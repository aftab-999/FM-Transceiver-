#include <xc.h>
#include <stdint.h>

// ================= CONFIG =================
#pragma config FOSC = HS
#pragma config WDTE = OFF
#pragma config PWRTE = OFF
#pragma config BOREN = OFF
#pragma config LVP = OFF
#pragma config CPD = OFF
#pragma config WRT = OFF
#pragma config CP = OFF

#define _XTAL_FREQ 20000000

// ================= PIN DEFINITIONS =================
#define PTT     RD2
#define BTN_UP  RC0
#define BTN_DN  RC1

#define OLED_CS   RD7
#define OLED_DC   RB0
#define OLED_RES  RB1

// ================= FREQUENCY LIMITS =================
// Stored as tenths of MHz: 100.0 MHz = 1000, 87.5 MHz = 875
#define FREQ_MIN  875
#define FREQ_MAX  1080
#define FREQ_DEFAULT 1000

// ================= GLOBAL =================
// uint16_t instead of float ? saves ~20-30 bytes RAM + eliminates soft-float lib
uint16_t frequency = FREQ_DEFAULT;

// MSSP mode tracking ? prevents redundant peripheral re-init
typedef enum { MODE_NONE = 0, MODE_I2C, MODE_SPI } mssp_mode_t;
static mssp_mode_t current_mode = MODE_NONE;

// ================= FUNCTION DECLARATIONS =================
void GPIO_Init(void);
void MSSP_I2C_Init(void);
void MSSP_SPI_Init(void);

void I2C_Start(void);
void I2C_Stop(void);
void I2C_Write(uint8_t data);

void TEA5767_SetFrequency(uint16_t freq);

void KT0803_WriteReg(uint8_t reg, uint8_t val);
void KT0803_Init(void);
void KT0803_SetFrequency(uint16_t freq);

void SPI_Write(uint8_t data);
void OLED_Command(uint8_t cmd);
void OLED_Data(uint8_t data);
void OLED_Init(void);
void OLED_SetCursor(uint8_t page, uint8_t col);
void OLED_PrintDigit(uint8_t d);
void OLED_PrintDot(void);
void OLED_PrintFrequency(uint16_t freq);

void Handle_Buttons(void);
void Transceiver_Update(void);

// ================= GPIO =================
void GPIO_Init(void)
{
    TRISB = 0x00;
    PORTB = 0x00;

    TRISC0 = 1;  // BTN_UP input
    TRISC1 = 1;  // BTN_DN input
    TRISD2 = 1;  // PTT input
    TRISD7 = 0;  // OLED_CS output
}

// ================= I2C =================
void MSSP_I2C_Init(void)
{
    // Only reinitialize if not already in I2C mode
    if(current_mode == MODE_I2C) return;

    TRISC3 = 1; // SCL
    TRISC4 = 1; // SDA

    SSPCON  = 0x28;
    SSPCON2 = 0x00;
    SSPADD  = (_XTAL_FREQ / (4UL * 100000UL)) - 1; // 100 kHz
    SSPSTAT = 0x00;

    current_mode = MODE_I2C;
}

void I2C_Start(void) { SEN = 1; while(SEN); }
void I2C_Stop(void)  { PEN = 1; while(PEN); }

void I2C_Write(uint8_t data)
{
    SSPBUF = data;
    while(!SSPIF);
    SSPIF = 0;
}

// ================= SPI =================
void MSSP_SPI_Init(void)
{
    // Only reinitialize if not already in SPI mode
    if(current_mode == MODE_SPI) return;

    TRISC3 = 0; // SCK output
    TRISC5 = 0; // SDO output

    SSPSTAT = 0x40;
    SSPCON  = 0x21;

    current_mode = MODE_SPI;
}

void SPI_Write(uint8_t data)
{
    SSPBUF = data;
    while(!SSPIF);
    SSPIF = 0;
}

// ================= TEA5767 (FM RECEIVER) =================
#define TEA5767_ADDR 0xC0

void TEA5767_SetFrequency(uint16_t freq)
{
    // freq in tenths of MHz (e.g. 1000 = 100.0 MHz)
    // freq_hz = freq * 100000
    // pll = 4 * (freq_hz + 225000) / 32768
    uint32_t freq_hz = (uint32_t)freq * 100000UL;
    uint16_t pll = (uint16_t)((4UL * (freq_hz + 225000UL)) / 32768UL);

    uint8_t d[5];
    d[0] = (uint8_t)((pll >> 8) & 0x3F);
    d[1] = (uint8_t)(pll & 0xFF);
    d[2] = 0xB0;  // Stereo, Mute off, High side injection
    d[3] = 0x10;  // 32.768 kHz crystal reference
    d[4] = 0x00;

    MSSP_I2C_Init(); // switches mode if needed

    I2C_Start();
    I2C_Write(TEA5767_ADDR);
    for(uint8_t i = 0; i < 5; i++) I2C_Write(d[i]);
    I2C_Stop();
}

// ================= KT0803 (FM TRANSMITTER) =================
#define KT0803_ADDR 0xD4

void KT0803_WriteReg(uint8_t reg, uint8_t val)
{
    MSSP_I2C_Init(); // switches mode if needed

    I2C_Start();
    I2C_Write(KT0803_ADDR);
    I2C_Write(reg);
    I2C_Write(val);
    I2C_Stop();
}

void KT0803_Init(void)
{
    KT0803_WriteReg(0x00, 0xC0);
    KT0803_WriteReg(0x01, 0x00);
}

void KT0803_SetFrequency(uint16_t freq)
{
    // freq in tenths of MHz
    // Original formula: ch = (freq_mhz - 70.0) * 10
    // Integer equivalent: ch = freq - 700
    // Example: 100.0 MHz ? 1000 - 700 = 300 ?
    //           87.5 MHz ?  875 - 700 = 175 ?
    uint16_t ch = freq - 700;

    KT0803_WriteReg(0x02, (uint8_t)((ch >> 8) & 0xFF));
    KT0803_WriteReg(0x03, (uint8_t)(ch & 0xFF));
}

// ================= OLED (SSD1306 via SPI) =================
// 5x7 font stored in Flash (const = program memory on XC8, costs 0 RAM)
const uint8_t font[10][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}  // 9
};

void OLED_Command(uint8_t c)
{
    MSSP_SPI_Init(); // switches mode if needed
    OLED_DC = 0;
    OLED_CS = 0;
    SPI_Write(c);
    OLED_CS = 1;
}

void OLED_Data(uint8_t d)
{
    MSSP_SPI_Init(); // switches mode if needed
    OLED_DC = 1;
    OLED_CS = 0;
    SPI_Write(d);
    OLED_CS = 1;
}

void OLED_Init(void)
{
    TRISB0 = 0; // DC
    TRISB1 = 0; // RES

    OLED_RES = 0;
    __delay_ms(50);
    OLED_RES = 1;
    __delay_ms(10);

    OLED_Command(0xAE); // Display off
    OLED_Command(0xA6); // Normal (non-inverted) display
    OLED_Command(0xAF); // Display on
}

void OLED_SetCursor(uint8_t p, uint8_t c)
{
    OLED_Command(0xB0 + p);
    OLED_Command(0x00 + (c & 0x0F));
    OLED_Command(0x10 + (c >> 4));
}

void OLED_PrintDigit(uint8_t d)
{
    for(uint8_t i = 0; i < 5; i++) OLED_Data(font[d][i]);
    OLED_Data(0x00); // 1-pixel spacing gap
}

void OLED_PrintDot(void)
{
    // 2-column decimal point, bits 5-6 lit (near bottom of glyph row)
    OLED_Data(0x60);
    OLED_Data(0x60);
    OLED_Data(0x00); // gap
}

void OLED_PrintFrequency(uint16_t freq)
{
    // freq range 875?1080
    // Display: "XXX.X" for >= 1000, " XX.X" for < 1000
    OLED_SetCursor(0, 0);

    if(freq >= 1000)
    {
        OLED_PrintDigit((uint8_t)(freq / 1000 % 10)); // hundreds (1)
    }
    else
    {
        // Print blank space so cursor advances same width
        for(uint8_t i = 0; i < 6; i++) OLED_Data(0x00);
    }

    OLED_PrintDigit((uint8_t)(freq / 100 % 10)); // tens
    OLED_PrintDigit((uint8_t)(freq / 10  % 10)); // ones
    OLED_PrintDot();
    OLED_PrintDigit((uint8_t)(freq % 10));        // tenths
}

// ================= LOGIC =================
void Handle_Buttons(void)
{
    if(BTN_UP == 0)
    {
        if(frequency < FREQ_MAX) frequency++;
        __delay_ms(200);
    }

    if(BTN_DN == 0)
    {
        if(frequency > FREQ_MIN) frequency++;
        __delay_ms(200);
    }
}

void Transceiver_Update(void)
{
    if(PTT == 0)
    {
        // PTT held: transmit mode (KT0803)
        KT0803_Init();
        KT0803_SetFrequency(frequency);
    }
    else
    {
        // PTT released: receive mode (TEA5767)
        TEA5767_SetFrequency(frequency);
    }
}

// ================= MAIN =================
void main(void)
{
    GPIO_Init();
    MSSP_SPI_Init();  // SPI first since OLED_Init needs it
    OLED_Init();

    while(1)
    {
        Handle_Buttons();
        Transceiver_Update();
        OLED_PrintFrequency(frequency);
        __delay_ms(200);
    }
}
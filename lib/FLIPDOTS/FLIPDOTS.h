#include <Arduino.h>

class FLIPDOTS
{
public:
    /**
     * Constructor.
     * @param serial - HardwareSerial the display is connected to.
     * @param address - The address of the display, by default it sends to all displays on the serial.
     */
    FLIPDOTS(HardwareSerial *serial, uint8_t address = 0xff, bool inverted = false) : Serial(serial), address(address), inverted(inverted) {}
    void begin(unsigned long baudRate = 9600, unsigned long ms = 1000);
    void update();
    void write3x3char4(const char *charArray);
    void write3x3char2andBars(const char *charArray, double bar1, double bar2, double bar3, bool flag);
    void write(const byte data[7], bool autoUpdate = true);
    void clear();
    void setInverted(bool inverted);

private:
    HardwareSerial *Serial;
    uint8_t address;
    bool inverted;
    const byte *get3x3FontGlyph(char c);
};
import RPi.GPIO as GPIO
import ctypes
import time

libc = ctypes.CDLL('libc.so.6')
def usleep(n):
    libc.usleep(n)
def delay(n):
    time.sleep(n/1000)

PARALLEL=False
USE_FRAMEBUFFER=True
if not PARALLEL: USE_FRAMEBUFFER=True
USE_CS = True

# set up pins
LCD_D2=2
LCD_D3=3
LCD_D4=4
LCD_XCS=17
LCD_RWWR=10
LCD_ERD=9
LCD_RST=11
LCD_D0=0
LCD_D5=5
LCD_D6=6
LCD_IF2=13
LCD_IF3=14
LCD_SI=15
LCD_A0=8
LCD_D7=7
LCD_D1=1
LCD_IF1=12
LCD_SCL=16
LCD_DATA=[LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_D4, LCD_D5, LCD_D6, LCD_D7]
# set up data bus, initially as Hi-Z
def init():
    GPIO.setmode(GPIO.BCM) # pin numbers on rasp pi header
    GPIO.setup(LCD_RST, GPIO.OUT, initial=GPIO.LOW)
    for pin in LCD_DATA:
        GPIO.setup(pin, GPIO.IN)
    # set up output pins
    # 68-series 8-bit parallel, or 9-bit SPI mode
    GPIO.setup(LCD_IF1, GPIO.OUT, initial=GPIO.LOW)
    GPIO.setup(LCD_IF2, GPIO.OUT, initial=GPIO.HIGH if PARALLEL else GPIO.LOW)
    GPIO.setup(LCD_IF3, GPIO.OUT, initial=GPIO.HIGH)
    GPIO.setup(LCD_A0, GPIO.OUT, initial=GPIO.LOW)
    GPIO.setup(LCD_RWWR, GPIO.OUT, initial=GPIO.HIGH)
    GPIO.setup(LCD_ERD, GPIO.OUT, initial=GPIO.HIGH)
    GPIO.setup(LCD_SI, GPIO.OUT, initial=GPIO.LOW)
    GPIO.setup(LCD_SCL, GPIO.OUT, initial=GPIO.LOW)
    GPIO.setup(LCD_XCS, GPIO.OUT, initial=GPIO.HIGH if USE_CS else GPIO.LOW)

    reset()

def write_serial(a0, val):
    GPIO.output(LCD_SCL, 1)
    if USE_CS: GPIO.output(LCD_XCS, 0)
    GPIO.output(LCD_SI, a0)
    GPIO.output(LCD_SCL, 0)
    GPIO.output(LCD_SCL, 1) # latch at rising edge
    for bit in range(8):
        GPIO.output(LCD_SCL, 0)
        GPIO.output(LCD_SI, val&0x80) # MSB first
        val = val << 1
        GPIO.output(LCD_SCL, 1) # latch at rising edge
    if USE_CS: GPIO.output(LCD_XCS, 1)

def write_parallel(a0, val):
    GPIO.output(LCD_A0, a0)
    GPIO.output(LCD_RWWR, 0)
    GPIO.output(LCD_ERD, 1)
    for pin in LCD_DATA:
        GPIO.setup(pin, GPIO.OUT, initial=(val&1))
        val = val >> 1
    GPIO.output(LCD_XCS, 0) # active low
    # data are latched at the falling edge of the E signal
    usleep(1)
    GPIO.output(LCD_ERD, 0)
    usleep(1) # actually 100ns
    GPIO.output(LCD_XCS, 1)
    GPIO.output(LCD_ERD, 1)
    for pin in LCD_DATA:
        GPIO.setup(pin, GPIO.IN)

def read_parallel(a0):
    GPIO.output(LCD_A0, a0)
    GPIO.output(LCD_RWWR, 1)
    GPIO.output(LCD_ERD, 1)
    GPIO.output(LCD_XCS, 0) # active low
    usleep(1) # 40ns
    val = 0
    for pin in reversed(LCD_DATA):
        val = val << 1
        if GPIO.input(pin): val = val | 0x1
    GPIO.output(LCD_XCS, 1)
    GPIO.output(LCD_ERD, 1)
    return val

def write_cmd(val):
    if PARALLEL:
        write_parallel(0, val)
    else:
        write_serial(0, val)
def write_data(val):
    if PARALLEL:
        write_parallel(1, val)
    else:
        write_serial(1, val)
def read_cmd():
    return read_parallel(0) if PARALLEL else 0
def read_data():
    return read_parallel(1) if PARALLEL else 0

def write_memory(data):
    write_cmd(0x5C)
    for byte in data:
        write_data(byte)

def read_memory(n):
    data = []
    write_cmd(0x5D)
    for i in range(n):
        data.append(read_data())
    return data

def dump_status():
    write_cmd( 0x0030 ) # Set Ext = 0
    write_cmd( 0x0025 ) # NOP
    st = read_cmd()
    print('Partial display:', 'ON' if (st&1) else 'OFF')
    print('Display:', 'Normal' if (st&2) else 'Inverse')
    print('EEPROM access:', 'In' if (st&4) else 'Out')
    print('Display:', 'ON' if (st&8) else 'OFF')
    print('Scan:', 'Line' if (st&16) else 'Column')
    print('RMW:', 'In' if (st&32) else 'Out')
    print('Area scroll mode:', st>>6)
    # "Read register 1" -- this is PB1 of the VOLCTRL cmd
    write_cmd(0x7C)
    low = read_cmd()
    # "Read register 2" -- this is PB2 of the VOLCTRL cmd
    write_cmd(0x7D)
    high = read_cmd()
    both = (low&0x3F) | (high<<6)
    v = 3.6 + (both)*0.04
    print('VOLCTRL:', f'{both:x} ({low:x}/{high:x}) {v}V')

def setvol(v):
    val = (v-3.6)/0.04
    val = int(val + 0.5)
    low = val & 0x3F
    high = val >> 6
    write_cmd( 0x0030 ) # Set Ext = 0
    write_cmd(0x81)
    write_data(low)
    write_data(high)

def reset():
    # in 68-series 8-bit parallel mode
    GPIO.output(LCD_RST, GPIO.LOW)
    usleep(1)
    GPIO.output(LCD_RST, GPIO.HIGH)
    usleep(2)
    write_cmd(0x0025) # NOP
    write_cmd(0x0025) # NOP
    dump_status()
    write_cmd( 0x0030 ) # Set Ext = 0
    write_cmd( 0x0094 ) # Exit sleep mode (SLPOUT)
    write_cmd( 0x00D1 ) # Internal Oscillator On (OSCON)
    write_cmd( 0x0020 ) # Power control set (PWRCTRL)
    write_data( 0x0008 ) # (Booster on, follower and reference off)
    delay( 5 ) # Booster must be on first before other power enabled
    write_cmd( 0x0020 ) # Power control set (PWRCTRL)
    write_data( 0x000B ) # (booster, follower & reference on)
    delay( 5 ) # Booster must be on first before other power enabled

    # "write contrast"
    write_cmd( 0x0081 ) # Program optimum LCD supply voltage (VOLCTRL)
    write_data( 0x0010 ) # VPR = 0b1 0001 0000 = 0x110 => 14.48V
    write_data( 0x0004 ) # (Reset state is 0x101 => Vop = 13.88V)

    write_cmd( 0x00CA ) # Display control (DISCTRL)
    write_data( 0x0000 ) # Clock divider = X1
    write_data( 0x0023 ) # Duty = 144
    write_data( 0x0000 ) # Frame=1 line cycle; FR Inverse-Set Value = 0
    write_cmd( 0x00A6 ) # Normal Display (DISNOR)
    write_cmd( 0x00BB ) # Common scan (COMSCN)
    write_data( 0x0002 )    # 79->0  80->159 (actually 63->0 80->143)
    # An alternative DATSDR is 3/1/2 with scroll(4) but there's an X offset
    write_cmd( 0x00BC ) # Data scan direction (DATSDR) (was: 0, 0, 2)
    write_data( 0x0000 ) # Address-scan= column, Column=normal, line=normal
    write_data( 0x0000 ) # RGB arrangement (not BGR)
    write_data( 0x0002 ) # 32 greyscale 3Byte 3Pixel mode
    write_cmd( 0x0075 ) # Line address set (LASET)
    write_data( 0x0000 ) # Start Line = 0
    write_data( 0x0077 ) # End Line = 119  (120 rows - 1)
    write_cmd( 0x0015 ) # Column address set (CASET)
    write_data( 0x0000 ) # Start Column = 0
    write_data( 0x004F ) # End Column = 79 (240 columns / 3 - 1)
    write_cmd( 0x0031 ) # Set Ext = 1
    write_cmd( 0x0032 ) # Analog Circuit Set (ANASET)
    write_data( 0x0000 ) # OSC Frequency = 000 (default, 12.7kHz)
    write_data( 0x0000 ) # Booster Efficiency = 0 (3k) (default = 6kHz)
    write_data( 0x0002 ) # Bias = 1/12
    write_cmd( 0x0034 ) # Software Initial (SWINT)

    # Gray table
    #write_cmd( 0x0020 ) # Gray 1 set
    #for i in range(0, 32, 2):
    #    write_data( i )
    #write_cmd( 0x0021 ) # Gray 2 set
    #for i in range(0, 32, 2):
    #    write_data( i )

    #ST7529_ReadEEPROM() # Read EEPROM Flow

    # Reset scroll
    scroll(36)
    invert() # so 0 is off and 31 = black
    on() # Display On (DISON)
    print()
    dump_status()
    # some additional stuff

def ST7529_ReadEEPROM():
    write_cmd( 0x0030 ) # EXT = 0
    write_cmd( 0x0007 ) # Initial code (1)
    write_data( 0x0019 )
    write_cmd( 0x0031 ) # EXT = 1
    write_cmd( 0x00CD ) # EEPROM ON
    write_data( 0x0000 ) # Enter "Read Mode"
    delay( 100 ) # Wait for EEPROM operation (100ms)
    write_cmd( 0x00FD ) # Start EEPROM reading operation
    delay( 100 ) # Wait for EEPROM operation (100ms)
    write_cmd( 0x00CC ) # Exit EEPROM mode
    write_cmd( 0x0030 ) # EXT = 0

def write_pattern():
    write_cmd( 0x0030 ) # Set Ext = 0
    write_cmd( 0x0015 ) # Column address set
    write_data( 0x0000 ) # From column 0
    write_data( 0x004F ) # To column 240 (aka 240/3 - 1)
    write_cmd( 0x0075 ) # Page address set
    write_data( 0x0000 ) # From line 0
    write_data( 0x0077 ) # To line 119
    write_cmd( 0x005C ) # Display Data Write
    for j in range(120):
        for i in range(79):
            write_data( 0 if (i&1) else 0xFF );
            write_data( i*31//78 );
            write_data( j*31//119 );

def x(col_from=0, col_to=1, line_from=0, line_to=1):
    # col_to may need to be divided by 3
    write_cmd( 0x0030 ) # Set Ext = 0
    write_cmd(0x15)
    write_data(col_from)
    write_data(col_to)
    write_cmd(0x75)
    write_data(line_from)
    write_data(line_to)

def invert(invert=True):
    write_cmd( 0x0030 ) # Set Ext = 0
    write_cmd(0xA7 if invert else 0xA6)

def on(on=True):
    write_cmd( 0x0030 ) # Set Ext = 0
    write_cmd(0xAF if on else 0xAE)

def boostfreq(x=0):
    write_cmd( 0x0031 ) # Set Ext = 1
    write_cmd( 0x0032 ) # Analog Circuit Set (ANASET)
    write_data( 0x0000 ) # OSC Frequency = 000 (default, 12.7kHz)
    write_data( x ) # Booster Efficiency = 0 (3k) (default = 6kHz)
    write_data( 0x0002 ) # Bias = 1/12
    write_cmd( 0x0030 ) # Set Ext = 0

def scroll(x=0):
    write_cmd( 0x0030 ) # Set Ext = 0
    write_cmd(0xAB)
    write_data(x)

COLS=240
ROWS=128
framebuffer = [0 for i in range(COLS*ROWS)] if USE_FRAMEBUFFER else None

def setpixel(x, y, level=0x1F):
    level = level << 3
    write_cmd( 0x0030 ) # Set Ext = 0
    write_cmd( 0x0015 ) # Column address set
    write_data( x // 3 )
    write_data( 0x4F )
    write_cmd( 0x0075 ) # Page address set
    write_data( y ) # From line 0
    write_data( 0x0077 ) # To line 119
    if USE_FRAMEBUFFER:
        framebuffer[x + (y*COLS)] = level
        write_cmd( 0x05C )
        for i in range(3):
            pixel = framebuffer[(x//3)*3 + (y*COLS) + i]
            write_data(pixel)
        return
    # otherwise framebuffer is in the display, use RMW
    write_cmd(0xE0) # enter RMW mode
    read_data() # dummy read
    pixel1 = read_data()
    pixel2 = read_data()
    pixel3 = read_data()
    p = (x % 3)
    if p == 0:
        pixel1 = level
    elif p == 1:
        pixel2 = level
    else:
        pixel3 = level
    write_data(pixel1)
    write_data(pixel2)
    write_data(pixel3)
    write_cmd(0xEE) # exit RMW mode

def clear():
    global framebuffer
    write_cmd( 0x0030 ) # Set Ext = 0
    write_cmd( 0x0015 ) # Column address set
    write_data( 0 )
    write_data( 84 )
    write_cmd( 0x0075 ) # Line address set
    write_data( 0 ) # From line 0
    write_data( 159 ) # To line 119
    write_cmd( 0x05C )
    for x in range(160*255):
        write_data(0)
    if USE_FRAMEBUFFER:
        framebuffer = [0 for i in range(COLS*ROWS)]

def gradient():
    #clear()
    for j in range(COLS):
        for i in range(ROWS):
            setpixel((i+j)%COLS,i,j&31)
def checkerbox():
    clear()
    for j in range(0,COLS,2):
        for i in range(0,ROWS,2):
            setpixel(j,i)
            setpixel((j+1)%COLS, (i+1)%ROWS)

def diagonal():
    clear()
    for i in range(min(ROWS,COLS)):
        setpixel(i,i)
    for i in range(COLS):
        setpixel(i,0)
    for i in range(ROWS):
        setpixel(0,i)

# this may not be necessary
GPIO.cleanup()

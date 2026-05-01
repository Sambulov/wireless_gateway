import serial

with serial.Serial('/dev/ttyUSB1', 115200) as s:
    index = 0
    while True:
        index += 1
        s.write(f"Hello terminal {index}".encode())
        s.write(b'\r\n')


import serial, time
s=serial.Serial('COM7',115200,timeout=0.2)
t=time.time()
while time.time()-t<25:
    d=s.read(4096)
    if d:
        print(d.decode('utf-8','replace'), end='')
s.close()

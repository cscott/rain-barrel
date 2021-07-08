#!/usr/bin/python3
import math

thresholdValue = 1.5
frequency = 19200 * 256

maxValue = -1000
minValue = 1000

lastTime = None
lastValue = None
with open('../snitch-capture-20210706-export/analog.csv', 'r') as file:
    file.readline() # throw away header
    while file:
        line = file.readline()
        if line == '': break
        line = line.split(',')
        seconds,level = float(line[0]), float(line[1])
        maxValue = max(level, maxValue)
        minValue = min(level, minValue)
        isHigh = (level > thresholdValue)
        timerCount = math.floor(seconds*frequency)
        if lastValue is None or (lastValue != isHigh and timerCount!=lastTime):
            #print(timerCount,1 if isHigh else 0)
            print(timerCount)
            #print (maxValue,minValue)
            lastValue = isHigh
            lastTime = timerCount
#print("Max:", maxValue)
#print("Min:", minValue)

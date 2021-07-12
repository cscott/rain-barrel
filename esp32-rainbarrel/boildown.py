#!/usr/bin/python3
import math

statistics=False
headerFile=False

# files and thresholds

#filename='../snitch-capture-20210706-export/analog.csv'
#thresholdValue = 1.5

#filename='../snitch-capture-20210708a-export/analog.csv'
#thresholdValue = 0.3

#filename='../snitch-capture-20210708b-export/digital.csv'
#thresholdValue = 0.5

#filename='../snitch-capture-20210708c-export/analog.csv'
#thresholdValue = 2.7

#filename='../snitch-capture-20210709a-export/digital.csv'
#thresholdValue = 0.5

#filename='../snitch-capture-20210709a-export/analog.csv'
#thresholdValue = 1.43 # this is best possible value
#thresholdValue = 1.77 # this is what RP2040 actually saw, more or less

#filename='../snitch-capture-20210709b-export/digital.csv'
#thresholdValue = 0.5

filename='../snitch-capture-20210709b-export/analog.csv'
thresholdValue = 2.0 # this is best possible value

filename='../snitch-capture-20210710-export/digital.csv'
thresholdValue = 0.5

#filename='../snitch-capture-202107010-export/analog.csv'
#thresholdValue = 1.5 # this shouldn't matter much

#filename='../snitch-capture-20210711-export/digital.csv'
#thresholdValue = 0.5

filename='../snitch-capture-20210711-export/analog.csv'
thresholdValue = None
thresholdValueLow = 1.0 # should be 0.8
thresholdValueHigh = 2.0

frequency = 19200 * 256

maxValue = -1000
minValue = 1000
avgSum = 0
avgCount = 0

lastTime = None
lastValue = None

if headerFile and not statistics:
    print("uint32_t timer_data[] = {");
with open(filename, 'r') as file:
    file.readline() # throw away header
    while file:
        line = file.readline()
        if line == '': break
        line = line.split(',')
        seconds,level = float(line[0]), float(line[1])
        isHigh = (level > thresholdValue) if thresholdValue is not None else \
          (level > thresholdValueHigh if lastValue == False else
           (level > thresholdValueLow) )
        timerCount = math.floor(seconds*frequency)
        if statistics:
            maxValue = max(level, maxValue)
            minValue = min(level, minValue)
            avgSum += level
            avgCount += 1
        elif lastValue is None or (lastValue != isHigh and timerCount!=lastTime):
            if headerFile:
                print("\t", timerCount, ",");
            else:
                #print(timerCount,1 if isHigh else 0)
                print(timerCount, ",")
            lastValue = isHigh
            lastTime = timerCount

if statistics:
    print("Max:", maxValue)
    print("Min:", minValue)
    print("Avg:", avgSum/avgCount)
elif headerFile:
    print("}");

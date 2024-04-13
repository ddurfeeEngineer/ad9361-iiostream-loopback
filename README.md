# ad9361-iiostream-loopback

## Description

ddurfee.engineer@gmail.com

I took the only file that Analog Devices provides as an example for streaming data with their AD9361. 

Actually there are two of the same name on the internet and only one of them compiles. 

https://github.com/analogdevicesinc/libiio/blob/main/examples/ad9361-iiostream.c 
It appears there is a newer version at because the above didn’t seem to compile.

https://analogdevicesinc.github.io/libiio/v0.23/libiio/ad9361-iiostream_8c-example.html

Here is the description of what it does:
“ad9361-iiostream is an example program that shows how to capture and transmit data using the IIO API and the AD9361. It first captures a buffer of data than swaps I and Q and transmits the same buffer. Is is not really a meaningful application, it's just to illustrate the usage of the API.”

This seemed rather useless so I modified the program so that I could do a real loopback.  (Not just setting a bit and using the internal loopback).  So I put a coax from tx chan 0 to rx chan 0.

I set the buffer size, the gain (rx 50 dB, rx -30dB, put the receiver into “manual gain set” mode), the carrier (2.45Ghz since the baluns are designed for that frequency), the sample rate (3MHz) , and I put a sine wave of frequency 50Khz into the transmit buffer.  

Then I looked at the data and….
•	There was a very slow noisy waveform and it was around 60Hz so I guessed that the receiver was coming on before the transmitter.
•	While I tried to delay the data capture to get rid of that, it seemed that I had to process and throw away about 1500 samples to get to “real” data.
•	Then, I extended the amount of received data I grabbed and noticed that the device transmitted more data than was in tx the data buffer.
o	It appears that the tx cycles through the buffer even if you set the cycle flag to FALSE.  I guess the 9361 doesn’t support not doing that?
•	I noticed that the transmit signal seems to change over time.  I guess it requires some settling time?
•	I grabbed even more data until the transmitter seemed to settle down.

The output file has that data.  Column A is I (inphase) and B is Q (quad).  I also computed and put the vector amplitude in column C and the instantaneous phase in column D.

What I am thinking.
I think that there looks like there is an offset to the signal.
It also seems like the phase is being changed every cycle.  (I probably have to sample faster to capture that better, I originally thought I was just capturing 50Khz).

So, I have some addition work to do but wanted to get this on github for others to use.


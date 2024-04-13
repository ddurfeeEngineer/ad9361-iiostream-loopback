// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * libiio - AD9361 IIO streaming example
 *
 * Copyright (C) 2014 IABG mbH
 * Author: Michael Feilen <feilen_at_iabg.de>
 * Modified: David Durfee (DAD) <durfee.engineer_at_gmail.com>
 *
 * Modifications were made so you can connect a coax fron tx
 * chan 0 to rx chan 0 to implement a "real"
 * loopback test.  There is a delay from tx to rx
 * that gave 60Hz noise (in the USA) during that time.
 * Timing didn't seem to avoid that so I through away data at
 * the beginning of the receiving of data.
 **/


#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <iio.h>
#include <math.h>
#include <tgmath.h>  // added this cause I had problems with sin()
#include <unistd.h>  // need this to be able to use "usleep()" if it becomes necessary for timing

/* helper macros */
#define MHZ(x) ((long long)(x*1000000.0 + .5))
#define GHZ(x) ((long long)(x*1000000000.0 + .5))

#define IIO_ENSURE(expr) { \
	if (!(expr)) { \
		(void) fprintf(stderr, "assertion failed (%s:%d)\n", __FILE__, __LINE__); \
		(void) abort(); \
	} \
}

/* RX is input, TX is output */
enum iodev { RX, TX };

/* common RX and TX streaming params */
struct stream_cfg {
	long long bw_hz; // Analog banwidth in Hz
	long long fs_hz; // Baseband sample rate in Hz
	long long lo_hz; // Local oscillator frequency in Hz
	long long gain;  // gain on rx, attenuation on tx
	const char* rfport; // Port name
};

/* static scratch mem for strings */
static char tmpstr[64];

/* IIO structs required for streaming */
static struct iio_context *ctx   = NULL;
static struct iio_channel *rx0_i = NULL;
static struct iio_channel *rx0_q = NULL;
static struct iio_channel *tx0_i = NULL;
static struct iio_channel *tx0_q = NULL;
static struct iio_buffer  *rxbuf = NULL;
static struct iio_buffer  *txbuf = NULL;

static bool stop;

/* cleanup and exit */
static void shutdown()
{
	printf("* Destroying buffers\n");
	if (rxbuf) { iio_buffer_destroy(rxbuf); }
	if (txbuf) { iio_buffer_destroy(txbuf); }

	printf("* Disabling streaming channels\n");
	if (rx0_i) { iio_channel_disable(rx0_i); }
	if (rx0_q) { iio_channel_disable(rx0_q); }
	if (tx0_i) { iio_channel_disable(tx0_i); }
	if (tx0_q) { iio_channel_disable(tx0_q); }

	printf("* Destroying context\n");
	if (ctx) { iio_context_destroy(ctx); }
	exit(0);
}

static void handle_sig(int sig)
{
	printf("Waiting for process to finish... Got signal %d\n", sig);
	stop = true;
}

/* check return value of attr_write function */
static void errchk(int v, const char* what) {
	 if (v < 0) { fprintf(stderr, "Error %d writing to channel \"%s\"\nvalue may not be supported.\n", v, what); shutdown(); }
}

/* write attribute: long long int */
static void wr_ch_lli(struct iio_channel *chn, const char* what, long long val)
{
	errchk(iio_channel_attr_write_longlong(chn, what, val), what);
}

// added by DAD to read before I write
static void rd_ch_lli(struct iio_channel *chn, const char* what, long long *val)
{
	errchk( iio_channel_attr_read_longlong(chn, what, val), what);
}

/* write attribute: string */
static void wr_ch_str(struct iio_channel *chn, const char* what, const char* str)
{
	errchk(iio_channel_attr_write(chn, what, str), what);
}

/* DAD added this read attribute: string */
static void rd_ch_str(struct iio_channel *chn, const char* what, char* str, size_t len)
{
	errchk(iio_channel_attr_read(chn, what, str, len), what);
}

/* helper function generating channel names */
static char* get_ch_name(const char* type, int id)
{
	snprintf(tmpstr, sizeof(tmpstr), "%s%d", type, id);
	return tmpstr;
}

/* returns ad9361 phy device */
static struct iio_device* get_ad9361_phy(void)
{
	struct iio_device *dev =  iio_context_find_device(ctx, "ad9361-phy");
	IIO_ENSURE(dev && "No ad9361-phy found");
	return dev;
}

/* finds AD9361 streaming IIO devices */
static bool get_ad9361_stream_dev(enum iodev d, struct iio_device **dev)
{
	switch (d) {
	case TX: *dev = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc"); return *dev != NULL;
	case RX: *dev = iio_context_find_device(ctx, "cf-ad9361-lpc");  return *dev != NULL;
	default: IIO_ENSURE(0); return false;
	}
}

/* finds AD9361 streaming IIO channels */
static bool get_ad9361_stream_ch(enum iodev d, struct iio_device *dev, int chid, struct iio_channel **chn)
{
	*chn = iio_device_find_channel(dev, get_ch_name("voltage", chid), d == TX);
	if (!*chn)
		*chn = iio_device_find_channel(dev, get_ch_name("altvoltage", chid), d == TX);
	return *chn != NULL;
}

/* finds AD9361 phy IIO configuration channel with id chid */
static bool get_phy_chan(enum iodev d, int chid, struct iio_channel **chn)
{
	switch (d) {
	case RX: *chn = iio_device_find_channel(get_ad9361_phy(), get_ch_name("voltage", chid), false); return *chn != NULL;
	case TX: *chn = iio_device_find_channel(get_ad9361_phy(), get_ch_name("voltage", chid), true);  return *chn != NULL;
	default: IIO_ENSURE(0); return false;
	}
}

/* finds AD9361 local oscillator IIO configuration channels */
static bool get_lo_chan(enum iodev d, struct iio_channel **chn)
{
	switch (d) {
	 // LO chan is always output, i.e. true
	case RX: *chn = iio_device_find_channel(get_ad9361_phy(), get_ch_name("altvoltage", 0), true); return *chn != NULL;
	case TX: *chn = iio_device_find_channel(get_ad9361_phy(), get_ch_name("altvoltage", 1), true); return *chn != NULL;
	default: IIO_ENSURE(0); return false;
	}
}

/* applies streaming configuration through IIO */
bool cfg_ad9361_streaming_ch(struct stream_cfg *cfg, enum iodev type, int chid)
{
	struct iio_channel *chn = NULL;
	long long ll_data;
	long long *ll_ptr = &ll_data;

	// Configure phy and lo channels
	printf("* Acquiring AD9361 phy channel %d\n", chid);
	if (!get_phy_chan(type, chid, &chn)) {	return false; }
	wr_ch_str(chn, "rf_port_select",     cfg->rfport);
	wr_ch_lli(chn, "rf_bandwidth",       cfg->bw_hz);
	wr_ch_lli(chn, "sampling_frequency", cfg->fs_hz);

// DAD looks like the gains aren't set so I'm setting them

	if (type == TX) {
	    wr_ch_lli(chn, "hardwaregain", cfg->gain);
	    rd_ch_lli(chn, "hardwaregain", ll_ptr);
		printf("* TX gain/attenuation value %lld\n", *ll_ptr);
	} else {
		char buf[1024];

// put it in manual mode to set the rx gain.
		wr_ch_str(chn, "gain_control_mode","manual");
		rd_ch_str(chn, "gain_control_mode", buf, sizeof(buf));
	    wr_ch_lli(chn, "hardwaregain", cfg->gain);
	    rd_ch_lli(chn, "hardwaregain", ll_ptr);
		printf("* RX gain is %lld, mode is %s\n", *ll_ptr, buf);
	}


	// Configure LO channel
	printf("* Acquiring AD9361 %s lo channel\n", type == TX ? "TX" : "RX");
	if (!get_lo_chan(type, &chn)) { return false; }
	wr_ch_lli(chn, "frequency", cfg->lo_hz);
	return true;
}

/* simple configuration and streaming */
/* usage:
 * Default context, assuming local IIO devices, i.e., this script is run on ADALM-Pluto for example
 $./a.out
 * URI context, find out the uri by typing `iio_info -s` at the command line of the host PC
 $./a.out usb:x.x.x
 */
int main (int argc, char **argv)
{
	// Streaming devices
	struct iio_device *tx;
	struct iio_device *rx;

	// RX sample counter
	size_t nrx = 0;

	// Stream configurations
	struct stream_cfg rxcfg;
	struct stream_cfg txcfg;

	// Listen to ctrl+c and IIO_ENSURE
	signal(SIGINT, handle_sig);

	// RX stream config
	// BW for filter to remove high end noise
	// rxcfg.bw_hz = MHZ(2);   // 2 MHz rf bandwidth
	rxcfg.bw_hz = MHZ(.5);
	// Sample rate of receiver
	//rxcfg.fs_hz = MHZ(2.5);   // 2.5 MS/s rx sample rate
	rxcfg.fs_hz = MHZ(3);
	// Carrier frequency
	rxcfg.lo_hz = GHZ(2.5); // 2.5 GHz rf frequency

	rxcfg.rfport = "A_BALANCED"; // port A (select for rf freq.)
	rxcfg.gain = 50;

	// TX stream config
	// BW for filter to remove high end noise
	// txcfg.bw_hz = MHZ(1.5); // 1.5 MHz rf bandwidth
	txcfg.bw_hz = MHZ(.5);
	// Sample rate that signal will be sent out at
	// txcfg.fs_hz = MHZ(2.5);   // 2.5 MS/s tx sample rate
	txcfg.fs_hz = MHZ(3);
	// Carrier frequency
	txcfg.lo_hz = GHZ(2.5); // 2.5 GHz rf frequency
	txcfg.rfport = "A"; // port A (select for rf freq.)
	txcfg.gain = -30;   // attentuation on the transmit channel.

	printf("* Acquiring IIO context\n");
	if (argc == 1) {
		IIO_ENSURE((ctx = iio_create_default_context()) && "No context");
	}
	else if (argc == 2) {
		IIO_ENSURE((ctx = iio_create_context_from_uri(argv[1])) && "No context");
	}
	IIO_ENSURE(iio_context_get_devices_count(ctx) > 0 && "No devices");

	printf("* Acquiring AD9361 streaming devices\n");
	IIO_ENSURE(get_ad9361_stream_dev(TX, &tx) && "No tx dev found");
	IIO_ENSURE(get_ad9361_stream_dev(RX, &rx) && "No rx dev found");

	printf("* Configuring AD9361 for streaming\n");
	IIO_ENSURE(cfg_ad9361_streaming_ch(&rxcfg, RX, 0) && "RX port 0 not found");
	IIO_ENSURE(cfg_ad9361_streaming_ch(&txcfg, TX, 0) && "TX port 0 not found");

	printf("* Initializing AD9361 IIO streaming channels\n");
	IIO_ENSURE(get_ad9361_stream_ch(RX, rx, 0, &rx0_i) && "RX chan i not found");
	IIO_ENSURE(get_ad9361_stream_ch(RX, rx, 1, &rx0_q) && "RX chan q not found");
	IIO_ENSURE(get_ad9361_stream_ch(TX, tx, 0, &tx0_i) && "TX chan i not found");
	IIO_ENSURE(get_ad9361_stream_ch(TX, tx, 1, &tx0_q) && "TX chan q not found");

	printf("* Enabling IIO streaming channels\n");
	iio_channel_enable(rx0_i);
	iio_channel_enable(rx0_q);
	iio_channel_enable(tx0_i);
	iio_channel_enable(tx0_q);

	printf("* Creating non-cyclic IIO buffers\n");
	rxbuf = iio_device_create_buffer(rx, 256, false);
	if (!rxbuf) {
		perror("Could not create RX buffer");
		shutdown();
	}
	// even though "cyclic mode" is defined as false below the tx seems to
	// continue cycle through the buffer forever
	txbuf = iio_device_create_buffer(tx, 256*4, false);
	if (!txbuf) {
		perror("Could not create TX buffer");
		shutdown();
	}

	// DAD let's create a couple of files so we can see what is transmitted/received
	FILE *foutp = fopen("output.csv", "w+");
	FILE *finp = fopen("input.csv", "w+");

	printf("* Starting IO streaming\n");

	ssize_t nbytes_rx, nbytes_tx;
	char *p_dat, *p_end;
	ptrdiff_t p_inc;
	int rx_loop;  // loop counter for receive.


	float freq = 2.0 * M_PI * 50.0e3;  // 2*pi*50KHz
	double ampl = 48; // peak value for a 12 bit value is 4096
		
	double i = 1. / txcfg.fs_hz;

	// WRITE: Get pointers to TX buf and write IQ to TX buf port 0
	p_inc = iio_buffer_step(txbuf);
	p_end = iio_buffer_end(txbuf);

// fill the transmit buffer with a sine wave.
	for (p_dat = (char *)iio_buffer_first(txbuf, tx0_i); p_dat < p_end; p_dat += p_inc) {
		// 12-bit sample needs to be MSB aligned so shift by 4
		// https://wiki.analog.com/resources/eval/user-guides/ad-fmcomms2-ebz/software/basic_iq_datafiles#binary_format

		// fill tx buffer with sine wave
		short ipart = 0;
		short qpart = ampl * cos(freq * i)*16; // to move the 12 bits to the MSB of the 16 bit array.

		((int16_t *)p_dat)[0] = ipart & 0xFFF0;
		((int16_t *)p_dat)[1] = qpart & 0xFFF0;

		// fill output file with the data so we can see what was sent.
		fprintf(finp, "%d, %d\n", ((int16_t*)p_dat)[0], ((int16_t*)p_dat)[1]);

		i += 1. / txcfg.fs_hz;
		}

	// Schedule TX buffer (start the transmission...)
	nbytes_tx = iio_buffer_push(txbuf);
	if (nbytes_tx < 0) { printf("Error pushing buf %d\n", (int) nbytes_tx); shutdown(); }


	//  RX buffer  (start the reception of data but throw the initial samples away till tx starts)
	for (rx_loop = 0; rx_loop < 2; rx_loop++) {
		//  RX buffer  (start the reception of data)
		nbytes_rx = iio_buffer_refill(rxbuf);
		if (nbytes_rx < 0) { printf("Error refilling buf %d\n",(int) nbytes_rx); shutdown(); }

		// READ: Get pointers to RX buf and read IQ from RX buf port 0
		p_inc = iio_buffer_step(rxbuf);
		p_end = iio_buffer_end(rxbuf);
		for (p_dat = (char *)iio_buffer_first(rxbuf, rx0_i); p_dat < p_end; p_dat += p_inc) {
			// throw away the data
			nrx++;
		}
	}

    printf("* data values dumped RX %d\n", nrx);
    nrx = 0;

    // Now start actually capturing data into the rx buffer a lot of times.
	for (rx_loop = 0; rx_loop < 40; rx_loop++) {
		//  RX buffer  (start the reception of data)
		nbytes_rx = iio_buffer_refill(rxbuf);
		if (nbytes_rx < 0) { printf("Error refilling buf %d\n",(int) nbytes_rx); shutdown(); }

		// READ: Get pointers to RX buf and read IQ from RX buf port 0
		p_inc = iio_buffer_step(rxbuf);
		p_end = iio_buffer_end(rxbuf);
		for (p_dat = (char *)iio_buffer_first(rxbuf, rx0_i); p_dat < p_end; p_dat += p_inc) {
			// grab the I and Q and dump it to a file
			const int16_t i = ((int16_t*)p_dat)[0]; // Real (I)
			const int16_t q = ((int16_t*)p_dat)[1]; // Imag (Q)

			nrx++;
			// how about also writing amplitude and phase in degrees to the file?
			fprintf(foutp, "%d, %d, %.4f, %.4f\n", i, q, (double)sqrt((i*i)+(q*q)), (180/M_PI)*atan((double)q/(double)i));
		}
	}

    printf("* data values received RX %d\n", nrx);
	fclose(finp);
	fclose(foutp);

	shutdown();

	return 0;
}
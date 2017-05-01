#include "../nps-miniHE_spl/spl.h"
#include <linux/types.h>


#define	EZSPI_DATA_FRAME_SIZE		8
#define	EZSPI_WAIT_FOR_BUSY_RETRY	0x100
#define MAX_WAIT_WR_ER              100000
#define EZ_SPI_FIFO_SIZE            16

/* SPI controller registers */
typedef struct {
	u32 ctrlr0; /* offset 0x00 */
	u32 ctrlr1;
	u32 ssienr;
	u32 mwcr;
	u32 ser; /* offset 0x10 */
	u32 baudr;
	u32 txftlr;
	u32 rxftlr;
	u32 txflr; /* offset 0x20 */
	u32 rxflr;
	u32 sr;
	u32 imr;
	u32 isr; /* offset 0x30 */
	u32 risr;
	u32 txoicr;
	u32 rxoicr;
	u32 rxuicr; /* offset 0x40 */
	u32 msticr;
	u32 icr;
	u32 dmacr;
	u32 dmatdlr; /* offset 0x50 */
	u32 dmardlr;
	u32 idr;
	u32 ssi_comp_version;
	/* Each address location is 16-bit only */
	u32 dr[36]; /* offset 0x60 */
	/* This register is 8-bit length */
	u32 rx_sample_dly; /* offset 0xF0 */
	u32 rsvd_0;
	u32 rsvd_1;
	u32 rsvd_2;
} ezspi_regs;

#define readb(addr)                 (*(volatile unsigned char *)(addr))
#define readw(addr)                 (*(volatile unsigned short *)(addr))
#define readl(addr)                 (*(volatile unsigned int *)(addr))
#define writeb(b,addr)              (*(volatile unsigned char *)(addr)) = (b)
#define writew(b,addr)              (*(volatile unsigned short *)(addr)) = (b)
#define writel(b,addr)              (*(volatile unsigned int *)(addr)) = (b)
#define ezspi_read32(_a_)			readl(_a_)
#define ezspi_write32(_a_,_v_)	    writel(_v_,_a_)
#define ezspi_clrsetbits32(_a_,_c_,_s_)	\
			 (ezspi_write32(_a_,(readl(_a_)&(~(_c_)) )|(_s_)))

static void ezspi_sendRecv(int byteLen, u8 *txd, u8 *rxd, ezspi_regs *regs);
static void spi_cs_activate(struct ezspi_slave *ss);
static void spi_cs_deactivate(struct ezspi_slave *ss);

static void spi_setup_slave(unsigned int clkDiv, struct ezspi_slave *ds);
static void readFlashBuf(struct ezspi_slave *dev, unsigned char *vec,
		int offset, int len);
static int spi_xfer(struct ezspi_slave *ss, unsigned int bitlen,
		const void *dout, void *din, unsigned long flags);
static void uart_core_init(void);
static void writeChar(char c);

unsigned long int prebootParams[4];

int spiFlash2RamGo(int coreNum, int src) {
	//	void (*fp)(int);
	struct ezspi_slave curSpi;
	struct ezspi_slave *dev = &curSpi;
	uart_core_init(); // init uart
	writeChar('$'); // write '$' on terminal
	spi_setup_slave(10, dev); // init SPI driver
	readFlashBuf(dev, (unsigned char *) prebootParams, src,
			sizeof(prebootParams)); // copy parameters
	readFlashBuf(dev, (unsigned char *) prebootParams[1], src, prebootParams[2]
			- prebootParams[1]); // copy
	//    fp = (void (*)(int))(prebootParams[3]);
	//    fp(coreNum); // go to execute the loaded program - not return
	return (coreNum);
}
static void uart_core_init(void) {
	REG_UART_LCR = 0x80; // DLAB on
	REG_UART_DLL = 17; // DLL (divisor = 31250000 / 115200 / 16)
	REG_UART_LCR = 0x3; // DLAB off, 8bit frame size
	REG_UART_FCR = 0x1; // FIFO enable
}

static void writeChar(char c) {
	while ((REG_UART_LSR & 0x20) == 0)
		; /* wait until Tx FIFO not full*/
	REG_UART_THR = c;
}

static void spi_setup_slave(unsigned int clkDiv, struct ezspi_slave *ds) {
	ds->freq = clkDiv;
	ds->mode = 0; /* Motorola mode */
	ds->txthrs = 0;
	ds->rxthrs = 0;
	ds->base = SSI_BASE;
	return;
}

/*
 * Routine readFlashBuf
 * read buffer from Flash
 * Input  - dev - device parameters
 *          offset - start address in Flash
 *          len - number of bytes to read
 * Output - vec - output vector minimum size is len bytes
 * Return Value - None
 *
 */

static void readFlashBuf(struct ezspi_slave *dev, unsigned char *vec,
		int offset, int len) {
	unsigned long page_addr;
	u8 cmd[10];
	page_addr = offset >> 8;

	cmd[0] = CMD_READ_ARRAY_FAST;
	cmd[1] = page_addr >> 8;
	cmd[2] = page_addr;
	cmd[3] = offset & 0x00FF;
	cmd[4] = 0x00;
	spi_xfer(dev, 5 * 8, cmd, NULL, SPI_XFER_BEGIN);
	spi_xfer(dev, len * 8, NULL, vec, SPI_XFER_END);
	return;
}

/*-----------------------------------------------------------------------
 * SPI transfer
 *
 * This writes "bitlen" bits out the SPI MOSI port and simultaneously clocks
 * "bitlen" bits in the SPI MISO port.  That's just the way SPI works.
 *
 * The source of the outgoing bits is the "dout" parameter and the
 * destination of the input bits is the "din" parameter.  Note that "dout"
 * and "din" can point to the same memory location, in which case the
 * input data overwrites the output data (since both are buffered by
 * temporary variables, this is OK).
 */
static int spi_xfer(struct ezspi_slave *ss, unsigned int bitlen,
		const void *dout, void *din, unsigned long flags) {
	ezspi_regs *regs;
	u8 *txd = (u8*) dout;
	u8 *rxd = (u8*) din;
	u32 i;
	int byteLen = bitlen / EZSPI_DATA_FRAME_SIZE;

	regs = (ezspi_regs *) ss->base;

	i = 0;
	while (ezspi_read32( &regs->sr ) & (EZSPI_SR_BUSY)) {
		if (i++ > EZSPI_WAIT_FOR_BUSY_RETRY) {
			/*			EPRINT( "Bus busy timeout\n" ); */
			return -1;
		}
		/*		udelay(1); */
	}

	if (flags & SPI_XFER_BEGIN) {
		spi_cs_activate(ss);
	}

	while (byteLen > 0) {
		i = EZ_SPI_FIFO_SIZE;
		if (i > byteLen) {
			i = byteLen;
		}
		ezspi_sendRecv(i, txd, rxd, (ezspi_regs *) regs);
		if (txd != NULL) {
			txd += i;
		}
		if (rxd != NULL) {
			rxd += i;
		}
		byteLen -= i;
	}

	if (flags & SPI_XFER_END) {
		spi_cs_deactivate(ss);
	}

	return (0);
}

/*****************************************************************************/

static void ezspi_sendRecv(int byteLen, u8 *txd, u8 *rxd, ezspi_regs *regs) {
	int k = byteLen;
	int n;
	volatile u32 tmp = 0;
	volatile u32 *fifo = &regs->dr[0];
	volatile u32 *status = &regs->sr;

	for (n = 0; n < k; n++) {
		if (txd != NULL)
			tmp = txd[n];
		ezspi_write32( fifo, tmp );
	}
	for (n = 0; n < k; n++) {
		while ((ezspi_read32( status ) & EZSPI_SR_RFNE) == 0)
			;
		tmp = ezspi_read32( fifo );
		if (rxd != NULL)
			rxd[n] = (u8) tmp;
	}
	return;
}

/*****************************************************************************/

static void spi_cs_activate(struct ezspi_slave *ss) {
	volatile ezspi_regs *regs = (volatile ezspi_regs *) ss->base;
	u32 mask = 1;

	/* Disable SSI to setup transfer parameters */
	ezspi_write32( &regs->ssienr, 0 );

	/* Enable slave */
	ezspi_write32( &regs->ser, mask );
	/* Set slave mode */
	/* Transiver mode = Transmit & Receive - bits 9:8, value 0
	 Frame Format   = Motorola SPI - bits 5:4, value 0 */
	mask = 0;
	/* Set Data Frame Size */
	mask |= (EZSPI_DATA_FRAME_SIZE - 1);
	ezspi_clrsetbits32( &regs->ctrlr0,
			EZSPI_CTRLR0_TMOD |
			EZSPI_CTRLR0_CPOL |
			EZSPI_CTRLR0_CPHA |
			EZSPI_CTRLR0_FRF |
			EZSPI_CTRLR0_DFS,
			mask );

	/* Set baudrate */
	ezspi_write32( &regs->baudr, ss->freq );
	ezspi_write32( &regs->ctrlr1, 0x0F );

	/* Set FIFO thresholds */
	ezspi_write32( &regs->txftlr, ss->txthrs );
	ezspi_write32( &regs->rxftlr, ss->rxthrs );
	ezspi_write32( &regs->ssienr, 1 );
	REG_CPU_SSI_MASK &= ~(CS_FLASH_OVER);
}

/*****************************************************************************/

static void spi_cs_deactivate(struct ezspi_slave *ss) {
	volatile ezspi_regs *regs = (volatile ezspi_regs *) ss->base;

	/* Disable SSI to setup transfer parameters */
	ezspi_write32( &regs->ssienr, 0 );
	/* Disable slave */
	ezspi_write32( &regs->ser, 0 );
	REG_CPU_SSI_MASK |= CS_FLASH_OVER;
}

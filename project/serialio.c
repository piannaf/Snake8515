/*
 * FILE: serialio.c
 *
 * Written by Peter Sutton.
 * 
 * Module to allow standard input/output routines to be used via 
 * serial port. The init_serial_stdio() method must be called before
 * any standard IO methods (e.g. printf). We use interrupt-based output
 * and a circular buffer to store output messages. (This allows us 
 * to print many characters at once to the buffer and have them 
 * output by the UART as speed permits.) If the buffer fills up, the
 * put method will block until there is room in it. 
 * Input is polling based - requesting input from stdin will block
 * until a character is available.
 * The function input_available() can be used to test whether there is
 * input available to read from stdin.
 *
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdio.h>

/* Clock rate in Hz. (The L at the end makes this a long constant (32 bit)
** as opposed to an integer constant (16 bit).) */
#define SYSCLK 4000000L

/* Global variables */
/* 
** Circular buffer to hold outgoing characters. The insert_pos variable
** keeps track of the position (0 to OUTPUT_BUFFER_SIZE-1) that the next
** outgoing character should be written to. bytes_in_buffer keeps
** count of the number of characters currently stored in the buffer 
** (ranging from 0 to OUTPUT_BUFFER_SIZE). This number of bytes immediately
** prior to the current insert_pos are the bytes waiting to be output.
** If the insert_pos reaches the end of the buffer it will wrap around
** to the beginning (assuming those bytes have been output).
*/
#define OUTPUT_BUFFER_SIZE 32
volatile char out_buffer[OUTPUT_BUFFER_SIZE];
volatile unsigned char out_insert_pos;
volatile unsigned char bytes_in_out_buffer;

/*
** Circular buffer to hold incoming characters. Works on same principle
** as output buffer
*/
#define INPUT_BUFFER_SIZE 16
volatile char input_buffer[INPUT_BUFFER_SIZE];
volatile unsigned char input_insert_pos;
volatile unsigned char bytes_in_input_buffer;
volatile unsigned char input_overrun;

/* Function prototypes */

void init_serial_stdio(long baudrate, char echo);
static int uart_put_char(char, FILE*);
static int uart_get_char(FILE*);

/* Setup a stream that uses the uart get and put functions. We will
** make standard input and output use this stream below.
*/
static FILE myStream = FDEV_SETUP_STREAM(uart_put_char, uart_get_char,
		_FDEV_SETUP_RW);

/* Global variables */
char do_echo;

void init_serial_stdio(long baudrate, char echo) {
	/*
	** Initialise our buffers
	*/
	out_insert_pos = 0;
	bytes_in_out_buffer = 0;
	input_insert_pos = 0;
	bytes_in_input_buffer = 0;
	input_overrun = 0;
	
	/*
	** Record whether we're going to echo characters or not
	*/
	do_echo = echo;
	
	/* Configure the serial port baud rate */
	/* (This differs from the datasheet formula so that we get 
	** rounding to the nearest integer while using integer division
	** (which truncates)).
	*/
	UBRR = ((SYSCLK / (8 * baudrate)) + 1)/2 - 1;
	
	/*
	** Enable transmission and receiving via UART. We don't enable
	** the UDR empty interrupt here (we wait until we've got a
	** character to transmit).
	** NOTE: Interrupts must be enabled globally for this
	** library to work, but we do not do this here.
	*/
	UCR = (1<<RXEN)|(1<<TXEN);
	
	/*
	** Enable receive complete interrupt 
	*/
	UCR  |= (1 <<RXCIE);

	/* Set up our stream so the put and get functions below are used 
	** to write/read characters via the serial port when we use
	** stdio functions
	*/
	stdout = &myStream;
	stdin = &myStream;
}

static int uart_put_char(char c, FILE* stream) {
	unsigned char interrupts_enabled;
	
	/* Add the character to the buffer for transmission (if there 
	** is space to do so). If not we wait until the buffer has space.
	** If the character is \n, we output \r (carriage return)
	** also.
	*/
	if(c == '\n') {
		uart_put_char('\r', stream);
	}
	
	/* 
	** Loop until the buffer has enough space. The bytes_in_buffer
	** variable will get modified by the ISR which extracts bytes
	** from the buffer.
	*/
	while(bytes_in_out_buffer >= OUTPUT_BUFFER_SIZE) {
		/* do nothing */
	}
	
	/* Add the character to the buffer for transmission if there
	** is space to do so. We advance the insert_pos to the next
	** character position. If this is beyond the end of the buffer
	** we wrap around back to the beginning of the buffer 
	** NOTE: we disable interrupts before modifying the buffer. This
	** prevents the ISR from modifying the buffer at the same time.
	** We reenable them if they were enabled when we entered the
	** function.
	*/
	
	interrupts_enabled = bit_is_set(SREG, SREG_I);
	cli();
	out_buffer[out_insert_pos++] = c;
	bytes_in_out_buffer++;
	if(out_insert_pos == OUTPUT_BUFFER_SIZE) {
		/* Wrap around buffer pointer if necessary */
		out_insert_pos = 0;
	}
	/* Reenable interrupts (UDR Empty interrupt may have been
	** disabled */
	UCR |= (1 << UDRIE);
	if(interrupts_enabled) {
		sei();
	}
	return 0;
}

int uart_get_char(FILE* stream) {
	/* Wait until we've received a character */
	while(bytes_in_input_buffer == 0) {
		/* do nothing */
	}
	
	/*
	** Turn interrupts off and remove a character from the input
	** buffer. We reenable interrupts if they were on.
	** The pending character is the one which is byte_in_input_buffer
	** characters before the insert position (taking into account
	** that we may need to wrap around).
	*/
	unsigned char interrupts_enabled = bit_is_set(SREG, SREG_I);
	cli();
	char c;
	if(input_insert_pos - bytes_in_input_buffer < 0) {
		/* Need to wrap around */
		c = input_buffer[input_insert_pos - bytes_in_input_buffer
				+ INPUT_BUFFER_SIZE];
	} else {
		c = input_buffer[input_insert_pos - bytes_in_input_buffer];
	}
	
	/* Decrement our count of bytes in the input buffer */
	bytes_in_input_buffer--;
	if(interrupts_enabled) {
		sei();
	}	
	return c;
}

int8_t input_available(void) {
	return (bytes_in_input_buffer != 0);
}

/*
 * Define the interrupt handler for UART Data Register Empty (i.e. 
 * another character can be taken from our buffer and written out)
 */

ISR(UART_UDRE_vect) 
{
	/* Check if we have data in our buffer */
	if(bytes_in_out_buffer > 0) {
		/* Yes we do - remove the pending byte and output it
		** via the UART. The pending byte (character) is the
		** one which is "bytes_in_buffer" characters before the 
		** insert_pos (taking into account that we may 
		** need to wrap around to the end of the buffer).
		*/
		char c;
		if(out_insert_pos - bytes_in_out_buffer < 0) {
			/* Need to wrap around */
			c = out_buffer[out_insert_pos - bytes_in_out_buffer
				+ OUTPUT_BUFFER_SIZE];
		} else {
			c = out_buffer[out_insert_pos - bytes_in_out_buffer];
		}
		/* Decrement our count of the number of bytes in the 
		** buffer 
		*/
		bytes_in_out_buffer--;
		
		/* Output the character via the UART */
		UDR = c;
	} else {
		/* No data in the buffer. We disable the UART Data
		** Register Empty interrupt because otherwise it 
		** will trigger again immediately this ISR exits. 
		** The interrupt is reenabled when a character is
		** placed in the buffer
		*/
		UCR &= ~(1<<UDRIE);
	}
}

/*
 * Define the interrupt handler for UART Receive Complete (i.e. 
 * we can read a character. The character is read and placed in
 * the input buffer. 
 *
 * NOTE: We ignore the possibility of hardware input overrun, i.e. a new
 * character arriving before the old one is read from the UDR I/O register.
 */

ISR(UART_RX_vect) 
{
	/* Read the character */
	char c;
	c = UDR;
		
	if(do_echo && bytes_in_out_buffer < OUTPUT_BUFFER_SIZE) {
		/* If echoing is enabled and there is output buffer
		** space, echo the received character back to the UART.
		** (If there is no output buffer space, characters
		** will be lost.)
		*/
		uart_put_char(c, 0);
	}
	
	/* 
	** Check if we have space in our buffer. If not, set the overrun
	** flag and throw away the character. (We never clear the 
	** overrun flag - it's up to the programmer to check/clear
	** this flag if desired.)
	*/
	if(bytes_in_input_buffer >= INPUT_BUFFER_SIZE) {
		input_overrun = 1;
	} else {
		/* If the character is a carriage return, turn it into a
		** linefeed 
		*/
		if (c == '\r') {
			c = '\n';
		}
		
		/* 
		** There is room in the input buffer 
		*/
		input_buffer[input_insert_pos++] = c;
		bytes_in_input_buffer++;
		if(input_insert_pos == INPUT_BUFFER_SIZE) {
			/* Wrap around buffer pointer if necessary */
			input_insert_pos = 0;
		}
	}
}

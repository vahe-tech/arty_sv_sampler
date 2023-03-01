/*******************************************************************
 * @file ps2_core.cpp
 *
 * @brief implementation of Ps2Core class
 *
 * @author p chu
 * @version v1.0: initial release
 ********************************************************************/
#define XPAR_XGPIO_NUM_INSTANCES 1

#include "xparameters.h"
//#include "microblaze_exception_handler.c"
#include "xil_exception.c"
#include "mb_interface.h"
#include "xil_assert.c"
//#include "xintc_g.c"
/*#include "xgpio_intr.c"
#include "xgpio_extra.c"
#include "xgpio_g.c"
#include "xgpio_sinit.c"
#include "xgpio.c"*/
#include "ps2_core.h"

// Define the address of the interrupt control register for the MicroBlaze processor
#define XPAR_AXI_GPIO_0_DEVICE_ID 0
#define INTC_DEVICE_ID 0

#define INTERRUPT_CONTROL_REG XPAR_IOMODULE_SINGLE_BASEADDR + XIN_ISR_OFFSET

Ps2Core::Ps2Core(uint32_t core_base_addr) {
   base_addr = core_base_addr;
}

Ps2Core::~Ps2Core() {
}

void Ps2Core::enqueue(uint8_t value) {
	if ((tail + 1) % QUEUE_SIZE == head) {
	// queue is full, do nothing
	return;
	}
	queue[tail] = value;
	queueCount++;
	tail = (tail + 1) % QUEUE_SIZE;
}

uint8_t Ps2Core::dequeue(void) {
	if (head == tail) {
	// queue is empty, do nothing
	return 0;
	}
	uint8_t value = queue[head];
	queueCount--;
	head = (head + 1) % QUEUE_SIZE;
	return value;
}

int Ps2Core::byte(uint32_t data) {
	return ((int) (data & RX_DATA_FIELD));
}

void Ps2Core::getPackets() {
    int data = 0;
    uint8_t byteArray[4] = {0x00, 0x00, 0x00, 0x00};
    int bytesProcessed = 0;
    while (bytesProcessed < 4) {
        if (bytesProcessed == 0 && (data = rx_word_from_byte()) != 0 && rx_idle(data) && (byte(data) & 0x08) == 0x08) {
        	if ((data & 0xF0) == 0xF0 || (data & 0xC0) >= 0x40) {//error or overflow on at least one of x, y
				bytesProcessed = 0;
				for (int idx = 0; idx < 4; idx++) {
					byteArray[idx] = 0x00;
					//lastSuccessfulPacket[idx] = 0x00;
				}
				//tx_byte(0xFC);  // Clear Mouse Buffer
				//tx_byte(0xF5);  // Disable Data Reporting
				// Flush Host Buffer
				while((data = rx_word_from_byte())  && byte(data) != 0)
					;
				//tx_byte(0xF4);  // Enable Data Reporting
				continue;
			}
        	else {
				byteArray[bytesProcessed++] = byte(data);
        	}
		}
        else if (bytesProcessed  && (data = rx_word_from_byte()) != 0 && rx_idle(data)) {
        	if ((bytesProcessed == 1 && (byte(data) & 0x80) >> 7 != (byteArray[0] & 0x10) >> 4) ||
        	    (bytesProcessed == 2 && (byte(data) & 0x80) >> 7 != (byteArray[0] & 0x20) >> 5) ||
        	    (bytesProcessed == 3 && ((byteArray[0] & 0x08) == 0x08) && byteArray[1] == 0x00 && byteArray[2] == 0x00 && (byte(data) == 0x00)) ||
				(bytesProcessed == 3 && ((byteArray[0] & 0x08) == 0x08) && byteArray[1] == 0x00 && byteArray[2] == 0x00 && (byte(data) & 0x0F) == 0x08) ||
				(bytesProcessed == 3 && (byte(data) & 0xC0) >= 0x40) ||
				(byte(data) & 0xF0) == 0xF0)
			    {//mismatch in sign, no data in x,y, or error (0xF0)
			    bytesProcessed = 0;
			    for (int idx = 0; idx < 4; idx++) {
			    	byteArray[idx] = 0x00;
			    	//lastSuccessfulPacket[idx] = 0x00;
			    }
				//tx_byte(0xFC);  // Clear Mouse Buffer
				//tx_byte(0xF5);  // Disable Data Reporting
				// Flush Host Buffer
				while((data = rx_word_from_byte())  && byte(data) != 0)
					;
				//tx_byte(0xF4);  // Enable Data Reporting
			    continue;
        	}
        	else {
           		byteArray[bytesProcessed++] = byte(data);
        	}
        }
    }
    for (int idx = 0; idx < 4; idx++) {
    	enqueue(byteArray[idx]);
 		hex(dir::RECV, byteArray[idx]);
    	//lastSuccessfulPacket[idx] = byteArray[idx];
    	byteArray[idx] = 0x00;
    }
    uart.disp((int)queueCount);
}

void Ps2Core::checkMovement() {
	Ps2Core::getMovementPackets();
}

void Ps2Core::getMovementPackets()  {
	Ps2Core::getPackets();
}

void Ps2Core::handleError(uint8_t *byteArray, uint8_t *lastSuccessfulPacket) {
	int last = 0;
    int data = 0;
    int recv = 0;
    int byt = 0;

	last = now_ms();
	hex(dir::SEND, tx_byte(0xFE)); //Resend
	while (byt < 4 && recv == lastSuccessfulPacket[byt++]) {
		while ((data = rx_word_from_byte()) == 0 && (now_ms() - last <= 1000))
			;
		last = now_ms();
		if (data != 0) {
			recv = hex(dir::RECV, byte(data));
		}
	}
	if (byt != 4) {
		byt = 0;
		hex(dir::SEND, tx_byte(0xFC)); //Clear
		//Wait for Acknowledge from Mouse
		last = now_ms();
		while (((data = rx_word_from_byte()) == 0 || hex(dir::RECV, data) != 0xFA) && (now_ms() - last <= 1000))
			;
		//Perform Reset Initialization
		while (init() != 2)
			;
	}

	for(int idx = 0; idx < 4; idx++)
		byteArray[idx] = 0x00;
}


int Ps2Core::rx_word_from_byte() {
	uint32_t data;
	data = io_read(base_addr, RD_DATA_REG);
	if (rx_fifo_empty(data))  // no data
	   return (0);
	else
	data = io_read(base_addr, RD_DATA_REG);
	io_write(base_addr, RM_RD_DATA_REG, 0); //dummy write to remove data from rx FIFO
	return ((int) data);
}


int Ps2Core::rx_byte() {
   uint32_t data;
   data = io_read(base_addr, RD_DATA_REG) & RX_DATA_FIELD;
   if (rx_fifo_empty(data))  // no data
        return (0);
   data = io_read(base_addr, RD_DATA_REG) & RX_DATA_FIELD;
   io_write(base_addr, RM_RD_DATA_REG, 0); //dummy write to remove data from rx FIFO
   return ((int) data);
}



int Ps2Core::rx_idle(uint32_t rd_word) {
   int idle;

   //rd_word = io_read(base_addr, RD_DATA_REG);
   idle = (int) (rd_word & RX_IDLE_FIELD) >> 9;
   return (idle);
}

uint8_t Ps2Core::tx_byte(uint8_t cmd) {
   io_write(base_addr, PS2_WR_DATA_REG, (uint32_t ) cmd);
   return cmd;
}

int Ps2Core::rx_fifo_empty(uint32_t rd_word) {
   int empty;
   //rd_word = io_read(base_addr, RD_DATA_REG);
   empty = (int) (rd_word & RX_EMPT_FIELD) >> 8;
   return (empty);
}


int Ps2Core::hex(dir direction = dir::SEND, int num = 0)
{
	//uart.disp("(");
	//uart.disp(num);
	//uart.disp(") ");
	if (direction == dir::RECV){
		uart.disp("    ");
	}
	uart.disp((direction == dir::RECV)? "Recv:": "Send:");
	uart.disp(" 0x");
    uart.disp("0123456789ABCDEF"[(int)(0x0F & (num >> 4))]);
    uart.disp("0123456789ABCDEF"[(int)(0x0F & num)]);
    uart.disp("\r\n");
    return num;
}

/* procedure:
 *    1. flush ps2 receiver fifo
 *    2. host sends reset command 0xff
 *    3. ps2 device acknowledges (0xfa) and performs self-test
 *    4. ps2 device responds 0xaa if test passes
 *    5a. keyboard sends no additional data
 *    5b. mouse sends an extra id 0x00
 *    6. host sends 0xf4 to start stream mode
 *    7. mouse acknowledges (0xfa)
 */

int Ps2Core::init() {
   uint32_t data;
   int last = 0;
   /* Flush fifo buffer */
   while((data = rx_word_from_byte())  && byte(data) != 0)
	   ;
   hex(dir::SEND, tx_byte(0xFF));  //Reset Mouse
   last = now_ms();
   while ((data = rx_word_from_byte()) == 0 && ((now_ms() - last) <= 1000));   last = now_ms();
   if (hex(dir::RECV, byte(data)) != 0xFA) return -1;//Check response (0xFA)
   while ((data = rx_word_from_byte()) == 0 && ((now_ms() - last) <= 1000));   last = now_ms();
   if (hex(dir::RECV, byte(data)) != 0xAA) return -2;//Check response (0xAA)
   while ((data = rx_word_from_byte()) == 0 && ((now_ms() - last) <= 1000));   last = now_ms();
   if (hex(dir::RECV, byte(data)) != 0x00) return -3;//Check response (0x00)
   hex(dir::SEND, tx_byte(0xF3)); //Set Sample Rate
   while ((data = rx_word_from_byte()) == 0 && ((now_ms() - last) <= 1000));   last = now_ms();
   if (hex(dir::RECV, byte(data)) != 0xFA) return -4;
   hex(dir::SEND, tx_byte(0xC8)); //Send 200
   while ((data = rx_word_from_byte()) == 0 && ((now_ms() - last) <= 1000));   last = now_ms();
   if (hex(dir::RECV, byte(data)) != 0xFA) return -5;
   hex(dir::SEND, tx_byte(0xF3)); //Set Sample Rate
   while ((data = rx_word_from_byte()) == 0 && ((now_ms() - last) <= 1000));   last = now_ms();
   if (hex(dir::RECV, byte(data)) != 0xFA) return -6;
   hex(dir::SEND, tx_byte(0x64)); //Send 100
   while ((data = rx_word_from_byte()) == 0 && ((now_ms() - last) <= 1000));   last = now_ms();
   if (hex(dir::RECV, byte(data)) != 0xFA) return -7;
   hex(dir::SEND, tx_byte(0xF3)); //Set Sample Rate
   while ((data = rx_word_from_byte()) == 0 && ((now_ms() - last) <= 1000));   last = now_ms();
   if (hex(dir::RECV, byte(data)) != 0xFA) return -8;
   hex(dir::SEND, tx_byte(0x50)); //Send 80
   while ((data = rx_word_from_byte()) == 0 && ((now_ms() - last) <= 1000));   last = now_ms();
   if (hex(dir::RECV, byte(data)) != 0xFA) return -9;
   hex(dir::SEND, tx_byte(0xF2)); //Read Device Type
   while ((data = rx_word_from_byte()) == 0 && ((now_ms() - last) <= 1000));   last = now_ms();
   if (hex(dir::RECV, byte(data)) != 0xFA) return -10;
   while ((data = rx_word_from_byte()) == 0 && ((now_ms() - last) <= 1000));   last = now_ms();
   if (hex(dir::RECV, byte(data)) != 0x03) return -11;
   hex(dir::SEND, tx_byte(0xEA)); //Set Enable State
   while ((data = rx_word_from_byte()) == 0 && ((now_ms() - last) <= 1000));   last = now_ms();
   if (hex(dir::RECV, byte(data)) != 0xFA) return -12;
   hex(dir::SEND, tx_byte(0xF4));  //Enable Data Reporting
   while ((data = rx_word_from_byte()) == 0 && ((now_ms() - last) <= 1000));   last = now_ms();
   if (hex(dir::RECV, byte(data)) != 0xFA) return -13;
   while((data = rx_word_from_byte())  && byte(data) != 0)
     ;
   return (2);  //Mouse Detected and Initialized Successfully
}
int Ps2Core::get_mouse_activity(int *lbtn, int *rbtn, int *xmov,
      int *ymov, int *zmov) {
   uint8_t b1, b2, b3, b4;

   /* retrieve bytes only if 4 or a multiple of 4 exist in queue */
   if (queueCount >= 4) {
	   b1 = dequeue();
	   b2 = dequeue();
	   b3 = dequeue();
	   b4 = dequeue();
   }
   else {
	   *lbtn = 0;
	   *rbtn = 0;
	   *xmov = 0;
	   *ymov = 0;
	   *zmov = 0;
	   return (0);
   }

   /* extract button info */
   *lbtn = (int) (b1 & 0x01);      // extract bit 0
   *rbtn = (int) (b1 & 0x02) >> 1; // extract bit 1
   /* extract x movement; manually convert 9-bit 2's comp to int */
   if (b1 & 0x10)                // check MSB (sign bit) of x movement
      *xmov = (b2 ^ 0xff) + 1;
   else
	  *xmov = b2; // data conversion
   /* extract y movement; manually convert 9-bit 2's comp to int */

   if (b1 & 0x20)                // check MSB (sign bit) of y movement
      *ymov = (b3 ^ 0xff) + 1;
   else
	  *ymov = b3; // data conversion

   if (b4 & 0x08)               // check MSB (sign bit) of z movement
      *zmov = ((b4 ^ 0x0f) & 0x0f) + 1;
   else
	  *zmov = b4 & 0x0f; // data conversion
   /* success */
   return (1);
}

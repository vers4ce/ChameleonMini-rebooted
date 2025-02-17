/*
 * MifareClassic.c
 *
 *  Created on: 13.05.2013
 *      Author: skuser
 */

#include "MifareClassic.h"

#include "ISO14443-3A.h"
#include "../Codec/ISO14443-2A.h"
#include "../Memory.h"
#include "Crypto1.h"
#include "../Random.h"
#include "../LED.h"

#define MFCLASSIC_1K_ATQA_VALUE     0x0004
#define MFPlus_ATQA_VALUE           0x0044
#define MFCLASSIC_4K_ATQA_VALUE     0x0002
#define MFCLASSIC_1K_SAK_CL1_VALUE  0x08
#define MFCLASSIC_4K_SAK_CL1_VALUE  0x18
#define SAK_CL1_VALUE				ISO14443A_SAK_INCOMPLETE
#define SAK_CL2_VALUE				ISO14443A_SAK_COMPLETE_NOT_COMPLIANT

#define MEM_UID_CL1_ADDRESS         0x00
#define MEM_UID_CL1_SIZE            4
#define MEM_UID_BCC1_ADDRESS        0x04
#define MEM_UID_CL2_ADDRESS        	0x03
#define MEM_UID_CL2_SIZE            4
#define MEM_KEY_A_OFFSET            48        /* Bytes */
#define MEM_KEY_B_OFFSET            58        /* Bytes */
#define MEM_KEY_BIGSECTOR_OFFSET	192
#define MEM_KEY_SIZE                6         /* Bytes */
#define MEM_ACC_GPB_SIZE            4         /* Bytes */
#define MEM_SECTOR_ADDR_MASK        0xFC
#define MEM_BIGSECTOR_ADDR_MASK		0xF0
#define MEM_BYTES_PER_BLOCK         16        /* Bytes */
#define MEM_VALUE_SIZE              4         /* Bytes */

#define ACK_NAK_FRAME_SIZE          4         /* Bits */
#define ACK_VALUE                   0x0A
#define NAK_INVALID_ARG             0x00
#define NAK_CRC_ERROR               0x01
#define NAK_NOT_AUTHED              0x04
#define NAK_EEPROM_ERROR            0x05
#define NAK_OTHER_ERROR             0x06

#define CMD_AUTH_A                  0x60
#define CMD_AUTH_B                  0x61
#define CMD_AUTH_FRAME_SIZE         2        /* Bytes without CRCA */
#define CMD_AUTH_RB_FRAME_SIZE      4        /* Bytes */
#define CMD_AUTH_AB_FRAME_SIZE      8        /* Bytes */
#define CMD_AUTH_BA_FRAME_SIZE      4        /* Bytes */
#define CMD_HALT                    0x50
#define CMD_HALT_FRAME_SIZE         2        /* Bytes without CRCA */
#define CMD_READ                    0x30
#define CMD_READ_FRAME_SIZE         2        /* Bytes without CRCA */
#define CMD_READ_RESPONSE_FRAME_SIZE 16      /* Bytes without CRCA */
#define CMD_WRITE                   0xA0
#define CMD_WRITE_FRAME_SIZE        2        /* Bytes without CRCA */
#define CMD_DECREMENT               0xC0
#define CMD_DECREMENT_FRAME_SIZE    2        /* Bytes without CRCA */
#define CMD_INCREMENT               0xC1
#define CMD_INCREMENT_FRAME_SIZE    2        /* Bytes without CRCA */
#define CMD_RESTORE                 0xC2
#define CMD_RESTORE_FRAME_SIZE      2        /* Bytes without CRCA */
#define CMD_TRANSFER                0xB0
#define CMD_TRANSFER_FRAME_SIZE     2        /* Bytes without CRCA */

/* Chinese magic backdoor commands (GEN 1A)*/
#define CMD_CHINESE_UNLOCK          0x40
#define CMD_CHINESE_WIPE            0x41
#define CMD_CHINESE_UNLOCK_RW       0x43

/*
Source: NXP: MF1S50YYX Product data sheet

Access conditions for the sector trailer

Access bits     Access condition for                   Remark
            KEYA         Access bits  KEYB
C1 C2 C3        read  write  read  write  read  write
0  0  0         never key A  key A never  key A key A  Key B may be read[1]
0  1  0         never never  key A never  key A never  Key B may be read[1]
1  0  0         never key B  keyA|B never never key B
1  1  0         never never  keyA|B never never never
0  0  1         never key A  key A  key A key A key A  Key B may be read,
                                                       transport configuration[1]
0  1  1         never key B  keyA|B key B never key B
1  0  1         never never  keyA|B key B never never
1  1  1         never never  keyA|B never never never

[1] For this access condition key B is readable and may be used for data
*/
#define ACC_TRAILOR_READ_KEYA   0x01
#define ACC_TRAILOR_WRITE_KEYA  0x02
#define ACC_TRAILOR_READ_ACC    0x04
#define ACC_TRAILOR_WRITE_ACC   0x08
#define ACC_TRAILOR_READ_KEYB   0x10
#define ACC_TRAILOR_WRITE_KEYB  0x20



/*
Access conditions for data blocks
Access bits Access condition for 				Application
C1 C2 C3 	read 	write 	increment 	decrement,
                                                transfer,
                                                restore

0 0 0 		key A|B key A|B key A|B 	key A|B 	transport configuration
0 1 0 		key A|B never 	never 		never 		read/write block
1 0 0 		key A|B key B 	never 		never 		read/write block
1 1 0 		key A|B key B 	key B 		key A|B 	value block
0 0 1 		key A|B never 	never 		key A|B 	value block
0 1 1 		key B 	key B 	never 		never 		read/write block
1 0 1 		key B 	never 	never 		never 		read/write block
1 1 1 		never 	never 	never 		never 		read/write block

*/
#define ACC_BLOCK_READ      0x01
#define ACC_BLOCK_WRITE     0x02
#define ACC_BLOCK_INCREMENT 0x04
#define ACC_BLOCK_DECREMENT 0x08

#define KEY_A 0
#define KEY_B 1

/*
// Decoding table for Access conditions of a data block
static const uint8_t abBlockAccessConditions[8][2] =
{
    //C1C2C3
    // 0 0 0 R:key A|B W: key A|B I:key A|B D:key A|B 	transport configuration
    {
        // Access with Key A
        ACC_BLOCK_READ | ACC_BLOCK_WRITE | ACC_BLOCK_INCREMENT | ACC_BLOCK_DECREMENT,
        // Access with Key B
        ACC_BLOCK_READ | ACC_BLOCK_WRITE | ACC_BLOCK_INCREMENT | ACC_BLOCK_DECREMENT
    },
    // 1 0 0 R:key A|B W:key B I:never D:never 	read/write block
    {
        // Access with Key A
        ACC_BLOCK_READ,
        // Access with Key B
        ACC_BLOCK_READ | ACC_BLOCK_WRITE
    },
    // 0 1 0 R:key A|B W:never I:never D:never 	read/write block
    {
        // Access with Key A
        ACC_BLOCK_READ,
        // Access with Key B
        ACC_BLOCK_READ
    },
    // 1 1 0 R:key A|B W:key B I:key B D:key A|B 	value block
    {
        // Access with Key A
        ACC_BLOCK_READ  |  ACC_BLOCK_DECREMENT,
        // Access with Key B
        ACC_BLOCK_READ | ACC_BLOCK_WRITE | ACC_BLOCK_INCREMENT | ACC_BLOCK_DECREMENT
    },
    // 0 0 1 R:key A|B W:never I:never D:key A|B 	value block
    {
        // Access with Key A
        ACC_BLOCK_READ  |  ACC_BLOCK_DECREMENT,
        // Access with Key B
        ACC_BLOCK_READ  |  ACC_BLOCK_DECREMENT
    },
    // 1 0 1 R:key B W:never I:never D:never 	read/write block
    {
        // Access with Key A
        0,
        // Access with Key B
        ACC_BLOCK_READ
    },
    // 0 1 1 R:key B W:key B I:never D:never	read/write block
    {
        // Access with Key A
        0,
        // Access with Key B
        ACC_BLOCK_READ | ACC_BLOCK_WRITE
    },
    // 1 1 1 R:never W:never I:never D:never	read/write block
    {
        // Access with Key A
        0,
        // Access with Key B
        0
    }

};
// Decoding table for Access conditions of the sector trailor
static const uint8_t abTrailorAccessConditions[8][2] =
{
    // 0  0  0 RdKA:never WrKA:key A  RdAcc:key A WrAcc:never  RdKB:key A WrKB:key A  	Key B may be read[1]
    {
        // Access with Key A
        ACC_TRAILOR_WRITE_KEYA | ACC_TRAILOR_READ_ACC | ACC_TRAILOR_WRITE_ACC | ACC_TRAILOR_READ_KEYB | ACC_TRAILOR_WRITE_KEYB,
        // Access with Key B
        0
    },
    // 1  0  0 RdKA:never WrKA:key B  RdAcc:keyA|B WrAcc:never RdKB:never WrKB:key B
    {
        // Access with Key A
        ACC_TRAILOR_READ_ACC,
        // Access with Key B
        ACC_TRAILOR_WRITE_KEYA | ACC_TRAILOR_READ_ACC |  ACC_TRAILOR_WRITE_KEYB
    },
    // 0  1  0 RdKA:never WrKA:never  RdAcc:key A WrAcc:never  RdKB:key A WrKB:never  Key B may be read[1]
    {
        // Access with Key A
        ACC_TRAILOR_READ_ACC | ACC_TRAILOR_READ_KEYB,
        // Access with Key B
        0
    },
    // 1  1  0         never never  keyA|B never never never
    {
        // Access with Key A
        ACC_TRAILOR_READ_ACC,
        // Access with Key B
        ACC_TRAILOR_READ_ACC
    },
    // 0  0  1         never key A  key A  key A key A key A  Key B may be read,transport configuration[1]
    {
        // Access with Key A
        ACC_TRAILOR_WRITE_KEYA | ACC_TRAILOR_READ_ACC | ACC_TRAILOR_WRITE_ACC | ACC_TRAILOR_READ_KEYB | ACC_TRAILOR_WRITE_KEYB,
        // Access with Key B
        0
    },
    // 0  1  1         never key B  keyA|B key B never key B
    {
        // Access with Key A
        ACC_TRAILOR_READ_ACC,
        // Access with Key B
        ACC_TRAILOR_WRITE_KEYA | ACC_TRAILOR_READ_ACC | ACC_TRAILOR_WRITE_ACC | ACC_TRAILOR_WRITE_KEYB
    },
    // 1  0  1         never never  keyA|B key B never never
    {
        // Access with Key A
        ACC_TRAILOR_READ_ACC,
        // Access with Key B
        ACC_TRAILOR_READ_ACC | ACC_TRAILOR_WRITE_ACC
    },
    // 1  1  1         never never  keyA|B never never never
    {
        // Access with Key A
        ACC_TRAILOR_READ_ACC,
        // Access with Key B
        ACC_TRAILOR_READ_ACC
    },
};
*/

static enum {
    STATE_HALT,
    STATE_IDLE,
    STATE_CHINESE_IDLE,
    STATE_CHINESE_WRITE,
    STATE_READY1,
	STATE_READY2,
    STATE_ACTIVE,
    STATE_AUTHING,
    STATE_AUTHED_IDLE,
    STATE_WRITE,
    STATE_INCREMENT,
    STATE_DECREMENT,
    STATE_RESTORE
} State;

static uint8_t CardResponse[4];
static uint8_t ReaderResponse[4];
static uint8_t CurrentAddress;
static uint8_t BlockBuffer[MEM_BYTES_PER_BLOCK];
static uint8_t AccessConditions[MEM_ACC_GPB_SIZE]; /* Access Conditions + General purpose Byte */
static uint8_t AccessAddress;
static uint16_t CardATQAValue;
static uint8_t CardSAKValue;

static uint8_t _7BUID = 0x00;
static bool FromHalt = false;

#define BYTE_SWAP(x) (((uint8_t)(x)>>4)|((uint8_t)(x)<<4))
#define NO_ACCESS 0x07

/* decode Access conditions for a block */
INLINE uint8_t GetAccessCondition(uint8_t Block)
{
    uint8_t  InvSAcc0;
    uint8_t  InvSAcc1;
    uint8_t  Acc0 = AccessConditions[0];
    uint8_t  Acc1 = AccessConditions[1];
    uint8_t  Acc2 = AccessConditions[2];
    uint8_t  ResultForBlock = 0;

    InvSAcc0 = ~BYTE_SWAP(Acc0);
    InvSAcc1 = ~BYTE_SWAP(Acc1);

    /* Check */
    if ( ((InvSAcc0 ^ Acc1) & 0xf0) ||   /* C1x */
         ((InvSAcc0 ^ Acc2) & 0x0f) ||   /* C2x */
         ((InvSAcc1 ^ Acc2) & 0xf0))     /* C3x */
    {
      return(NO_ACCESS);
    }
    /* Fix for MFClassic 4K cards */
    if(Block<128)
        Block &= 3;
    else {
        Block &= 15;
        if (Block& 15)
            Block=3;
        else if (Block<=4)
            Block=0;
        else if (Block<=9)
            Block=1;
        else
            Block=2;
    }

    Acc0 = ~Acc0;       /* C1x Bits to bit 0..3 */
    Acc1 =  Acc2;       /* C2x Bits to bit 0..3 */
    Acc2 =  Acc2 >> 4;  /* C3x Bits to bit 0..3 */

    if(Block)
    {
        Acc0 >>= Block;
        Acc1 >>= Block;
        Acc2 >>= Block;
    }
    /* combine the bits */
    ResultForBlock = ((Acc2 & 1) << 2) |
                     ((Acc1 & 1) << 1) |
                     (Acc0 & 1);
    return(ResultForBlock);
}

INLINE bool CheckValueIntegrity(uint8_t* Block)
{
    /* Value Blocks contain a value stored three times, with
     * the middle portion inverted. */
    if (    (Block[0] == (uint8_t) ~Block[4]) && (Block[0] == Block[8])
         && (Block[1] == (uint8_t) ~Block[5]) && (Block[1] == Block[9])
         && (Block[2] == (uint8_t) ~Block[6]) && (Block[2] == Block[10])
         && (Block[3] == (uint8_t) ~Block[7]) && (Block[3] == Block[11])
         && (Block[12] == (uint8_t) ~Block[13])
         && (Block[12] == Block[14])
         && (Block[14] == (uint8_t) ~Block[15])) {
        return true;
    } else {
        return false;
    }
}

INLINE void ValueFromBlock(uint32_t* Value, uint8_t* Block)
{
    *Value = 0;
    *Value |= ((uint32_t) Block[0] << 0);
    *Value |= ((uint32_t) Block[1] << 8);
    *Value |= ((uint32_t) Block[2] << 16);
    *Value |= ((uint32_t) Block[3] << 24);
}

INLINE void ValueToBlock(uint8_t* Block, uint32_t Value)
{
    Block[0] = (uint8_t) (Value >> 0);
    Block[1] = (uint8_t) (Value >> 8);
    Block[2] = (uint8_t) (Value >> 16);
    Block[3] = (uint8_t) (Value >> 24);
    Block[4] = ~Block[0];
    Block[5] = ~Block[1];
    Block[6] = ~Block[2];
    Block[7] = ~Block[3];
    Block[8] = Block[0];
    Block[9] = Block[1];
    Block[10] = Block[2];
    Block[11] = Block[3];
}

void MifareClassicAppInit1K(void)
{
    State = STATE_IDLE;
    CardATQAValue = MFCLASSIC_1K_ATQA_VALUE;
    CardSAKValue = MFCLASSIC_1K_SAK_CL1_VALUE;
	_7BUID = 0x00;
}

void MifarePlus1kAppInit_7B(void)
{
	State = STATE_IDLE;
	CardATQAValue = MFPlus_ATQA_VALUE;
	CardSAKValue = MFCLASSIC_1K_SAK_CL1_VALUE;
	uint8_t UidSize = ActiveConfiguration.UidSize;
	_7BUID = (UidSize == 7);
}

void MifareClassicAppInit4K(void)
{
    State = STATE_IDLE;
    CardATQAValue = MFCLASSIC_4K_ATQA_VALUE;
    CardSAKValue = MFCLASSIC_4K_SAK_CL1_VALUE;
	_7BUID = 0x00;
}

void MifareClassicAppReset(void)
{
    State = STATE_IDLE;
}

void MifareClassicAppTask(void)
{

}


uint16_t MifareClassicAppProcess(uint8_t* Buffer, uint16_t BitCount)
{
    /* Wakeup and Request may occure in all states */
    if ( (BitCount == 7) &&
         /* precheck of WUP/REQ because ISO14443AWakeUp destroys BitCount */
         (((State != STATE_HALT) && (Buffer[0] == ISO14443A_CMD_REQA)) ||
         (Buffer[0] == ISO14443A_CMD_WUPA) )){

        if (ISO14443AWakeUp(Buffer, &BitCount, CardATQAValue, FromHalt)) {
            AccessAddress = 0xff;
            State = STATE_READY1;
            return BitCount;
        }
    }

    switch(State) {
    case STATE_IDLE:
    case STATE_HALT:

        if (ISO14443AWakeUp(Buffer, &BitCount, CardATQAValue, true)) {
            State = STATE_READY1;
            return BitCount;
        }
#ifdef SUPPORT_MF_CLASSIC_MAGIC_MODE
        else if (Buffer[0] == CMD_CHINESE_UNLOCK) {
            State = STATE_CHINESE_IDLE;
            Buffer[0] = ACK_VALUE;
            return ACK_NAK_FRAME_SIZE;
        }
#endif
        break;

#ifdef SUPPORT_MF_CLASSIC_MAGIC_MODE
    case STATE_CHINESE_IDLE:
        /* Support special china commands that dont require authentication. */
        if (Buffer[0] == CMD_CHINESE_UNLOCK_RW) {
            /* Unlock read and write commands */
            Buffer[0] = ACK_VALUE;
            return ACK_NAK_FRAME_SIZE;
        } else if (Buffer[0] == CMD_CHINESE_WIPE) {
            /* Wipe memory */
            Buffer[0] = ACK_VALUE;
            return ACK_NAK_FRAME_SIZE;
        } else if (Buffer[0] == CMD_READ) {
            if (ISO14443ACheckCRCA(Buffer, CMD_READ_FRAME_SIZE)) {
                /* Read command. Read data from memory and append CRCA. */
                MemoryReadBlock(Buffer, (uint16_t) Buffer[1] * MEM_BYTES_PER_BLOCK, MEM_BYTES_PER_BLOCK);
                ISO14443AAppendCRCA(Buffer, MEM_BYTES_PER_BLOCK);

                return (CMD_READ_RESPONSE_FRAME_SIZE + ISO14443A_CRCA_SIZE )
                        * BITS_PER_BYTE;
            } else {
                Buffer[0] = NAK_CRC_ERROR;
                return ACK_NAK_FRAME_SIZE;
            }
        } else if (Buffer[0] == CMD_WRITE) {
            if (ISO14443ACheckCRCA(Buffer, CMD_WRITE_FRAME_SIZE)) {
                /* Write command. Store the address and prepare for the upcoming data.
                * Respond with ACK. */
                CurrentAddress = Buffer[1];
                State = STATE_CHINESE_WRITE;

                Buffer[0] = ACK_VALUE;
                return ACK_NAK_FRAME_SIZE;
            } else {
                Buffer[0] = NAK_CRC_ERROR;
                return ACK_NAK_FRAME_SIZE;
            }
        } else if (Buffer[0] == CMD_HALT) {
            /* Halts the tag. According to the ISO14443, the second
            * byte is supposed to be 0. */
            if (Buffer[1] == 0) {
                if (ISO14443ACheckCRCA(Buffer, CMD_HALT_FRAME_SIZE)) {
                    /* According to ISO14443, we must not send anything
                    * in order to acknowledge the HALT command. */
                    State = STATE_HALT;
                    return ISO14443A_APP_NO_RESPONSE;
                } else {
                    Buffer[0] = NAK_CRC_ERROR;
                    return ACK_NAK_FRAME_SIZE;
                }
            } else {
                Buffer[0] = NAK_INVALID_ARG;
                return ACK_NAK_FRAME_SIZE;
            }
        }
        break;

    case STATE_CHINESE_WRITE:
        if (ISO14443ACheckCRCA(Buffer, MEM_BYTES_PER_BLOCK)) {
            /* CRC check passed. Write data into memory and send ACK. */
            if (!ActiveConfiguration.ReadOnly) {
                MemoryWriteBlock(Buffer, CurrentAddress * MEM_BYTES_PER_BLOCK, MEM_BYTES_PER_BLOCK);
            }

            Buffer[0] = ACK_VALUE;
        } else {
            /* CRC Error. */
            Buffer[0] = NAK_CRC_ERROR;
        }

        State = STATE_CHINESE_IDLE;

        return ACK_NAK_FRAME_SIZE;
#endif

    case STATE_READY1:
        if (ISO14443AWakeUp(Buffer, &BitCount, CardATQAValue, false)) {
            State = STATE_READY1;
            return BitCount;
        } else if (Buffer[0] == ISO14443A_CMD_SELECT_CL1) {
            /* Load UID CL1 and perform anticollision */
            uint8_t UidCL1[ISO14443A_CL_UID_SIZE];

            if (_7BUID) {
	            MemoryReadBlock(&UidCL1[1], MEM_UID_CL1_ADDRESS, MEM_UID_CL1_SIZE-1);
	            UidCL1[0] = ISO14443A_UID0_CT;

	            if (ISO14443ASelect(Buffer, &BitCount, UidCL1, SAK_CL1_VALUE))
				    State = STATE_READY2;

	        } else {
                MemoryReadBlock(UidCL1, MEM_UID_CL1_ADDRESS, MEM_UID_CL1_SIZE);
                if (ISO14443ASelect(Buffer, &BitCount, UidCL1, CardSAKValue)) {
                    AccessAddress = 0xff; /* invalid, force reload */
                    State = STATE_ACTIVE;
                }
            }
            return BitCount;
        } else {
            /* Unknown command. Enter HALT state. */
            State = STATE_HALT;
        }
        break;

    case STATE_READY2:
        if (ISO14443AWakeUp(Buffer, &BitCount, CardATQAValue, false)) {
	       State = STATE_READY1;
	       return BitCount;
	    } else if (Buffer[0] == ISO14443A_CMD_SELECT_CL2) {

	       /* Load UID CL2 and perform anticollision */
	       uint8_t UidCL2[ISO14443A_CL_UID_SIZE];
	       MemoryReadBlock(UidCL2, MEM_UID_CL2_ADDRESS, MEM_UID_CL2_SIZE);

	       if (ISO14443ASelect(Buffer, &BitCount, UidCL2, CardSAKValue)) {
               AccessAddress = 0xff; /* invalid, force reload */
		       State = STATE_ACTIVE;
	       }
		   return BitCount;
	    } else {
          /* Unknown command. Enter HALT state. */
          State = STATE_HALT;
        }
    break;

    case STATE_ACTIVE:
        if (ISO14443AWakeUp(Buffer, &BitCount, CardATQAValue, false)) {
            State = STATE_READY1;
            return BitCount;
        } else if (Buffer[0] == CMD_HALT) {

            /* Halts the tag. According to the ISO14443, the second
            * byte is supposed to be 0. */
            if (Buffer[1] == 0) {
                if (ISO14443ACheckCRCA(Buffer, CMD_HALT_FRAME_SIZE)) {
                    /* According to ISO14443, we must not send anything
                    * in order to acknowledge the HALT command. */
                    State = STATE_HALT;
                    return ISO14443A_APP_NO_RESPONSE;
                } else {
                    Buffer[0] = NAK_CRC_ERROR;
                    return ACK_NAK_FRAME_SIZE;
                }
            } else {
                Buffer[0] = NAK_INVALID_ARG;
                return ACK_NAK_FRAME_SIZE;
            }
        } else if ( (Buffer[0] == CMD_AUTH_A) || (Buffer[0] == CMD_AUTH_B)) {
            if (ISO14443ACheckCRCA(Buffer, CMD_AUTH_FRAME_SIZE)) {
                uint8_t SectorAddress;
                uint8_t KeyOffset = (Buffer[0] == CMD_AUTH_A ? MEM_KEY_A_OFFSET : MEM_KEY_B_OFFSET);
                /* Fix for MFClassic 4k cards */
                if(Buffer[1] >= 128) {
                    SectorAddress = Buffer[1] & MEM_BIGSECTOR_ADDR_MASK;
                    KeyOffset += MEM_KEY_BIGSECTOR_OFFSET;
                } else {
                    SectorAddress = Buffer[1] & MEM_SECTOR_ADDR_MASK;
                }
                uint16_t KeyAddress = (uint16_t) SectorAddress * MEM_BYTES_PER_BLOCK + KeyOffset;
                uint8_t Key[6];
                uint8_t Uid[4];
			    uint8_t CardNonce[4] = {0x01,0x20,0x01,0x45};
                uint8_t CardNonceSuccessor1[4] = {0x63, 0xe5, 0xbc, 0xa7};
                uint8_t CardNonceSuccessor2[4] = {0x99, 0x37, 0x30, 0xbd};

                /* Generate a random nonce and read UID and key from memory */
                //RandomGetBuffer(CardNonce, sizeof(CardNonce));
                if (_7BUID)
					MemoryReadBlock(Uid, MEM_UID_CL2_ADDRESS, MEM_UID_CL2_SIZE);
                else
                    MemoryReadBlock(Uid, MEM_UID_CL1_ADDRESS, MEM_UID_CL1_SIZE);

                MemoryReadBlock(Key, KeyAddress, MEM_KEY_SIZE);

                /* Precalculate the reader response from card-nonce */
                memcpy(ReaderResponse, CardNonceSuccessor1, 4);

                /* Precalculate our response from the reader response */
                memcpy(CardResponse, CardNonceSuccessor2, 4);

                /* Respond with the random card nonce and expect further authentication
                * form the reader in the next frame. */
                State = STATE_AUTHING;

                for (uint8_t i=0; i<sizeof(CardNonce); i++)
                    Buffer[i] = CardNonce[i];

                /* Setup crypto1 cipher. Discard in-place encrypted CardNonce. */
                Crypto1Setup(Key, Uid, CardNonce);

                return CMD_AUTH_RB_FRAME_SIZE * BITS_PER_BYTE;
            } else {
                Buffer[0] = NAK_CRC_ERROR;
                return ACK_NAK_FRAME_SIZE;
            }
        } else if ( (Buffer[0] == CMD_READ) ||
		            (Buffer[0] == CMD_WRITE) ||
					(Buffer[0] == CMD_DECREMENT) ||
					(Buffer[0] == CMD_INCREMENT) ||
					(Buffer[0] == CMD_RESTORE) ||
					(Buffer[0] == CMD_TRANSFER) ) {
            State = STATE_IDLE;
            Buffer[0] = NAK_NOT_AUTHED;
            return ACK_NAK_FRAME_SIZE;
        } else {
            /* Unknown command. Enter HALT state. */
            State = STATE_IDLE;
            return ISO14443A_APP_NO_RESPONSE;
        }
        break;

    case STATE_AUTHING:
        /* Reader delivers an encrypted nonce. We use it
        * to setup the crypto1 LFSR in nonlinear feedback mode.
        * Furthermore it delivers an encrypted answer. Decrypt and check it */
        Crypto1Auth(&Buffer[0]);

        for (uint8_t i=0; i<4; i++)
            Buffer[i+4] ^= Crypto1Byte();

        if ((Buffer[4] == ReaderResponse[0]) &&
            (Buffer[5] == ReaderResponse[1]) &&
            (Buffer[6] == ReaderResponse[2]) &&
            (Buffer[7] == ReaderResponse[3])) {
            /* Reader is authenticated. Encrypt the precalculated card response
            * and generate the parity bits. */
            for (uint8_t i=0; i<sizeof(CardResponse); i++) {
                Buffer[i] = CardResponse[i] ^ Crypto1Byte();
                Buffer[ISO14443A_BUFFER_PARITY_OFFSET + i] = ODD_PARITY(CardResponse[i]) ^ Crypto1FilterOutput();
            }

            State = STATE_AUTHED_IDLE;

            return (CMD_AUTH_BA_FRAME_SIZE * BITS_PER_BYTE) | ISO14443A_APP_CUSTOM_PARITY;
        } else {
            /* Just reset on authentication error. */
            State = STATE_IDLE;
        }

        break;

    case STATE_AUTHED_IDLE:
        /* In this state, all communication is encrypted. Thus we first have to encrypt
        * the incoming data. */
        for (uint8_t i=0; i<4; i++)
            Buffer[i] ^= Crypto1Byte();

        if (Buffer[0] == CMD_READ) {
            if (ISO14443ACheckCRCA(Buffer, CMD_READ_FRAME_SIZE)) {
                /* Read command. Read data from memory and append CRCA. */
                MemoryReadBlock(Buffer, (uint16_t) Buffer[1] * MEM_BYTES_PER_BLOCK, MEM_BYTES_PER_BLOCK);
                ISO14443AAppendCRCA(Buffer, MEM_BYTES_PER_BLOCK);

                /* Encrypt and calculate parity bits. */
                for (uint8_t i=0; i<(ISO14443A_CRCA_SIZE + MEM_BYTES_PER_BLOCK); i++) {
                    uint8_t Plain = Buffer[i];
                    Buffer[i] = Plain ^ Crypto1Byte();
                    Buffer[ISO14443A_BUFFER_PARITY_OFFSET + i] = ODD_PARITY(Plain) ^ Crypto1FilterOutput();
                }

                return ( (CMD_READ_RESPONSE_FRAME_SIZE + ISO14443A_CRCA_SIZE ) * BITS_PER_BYTE) | ISO14443A_APP_CUSTOM_PARITY;
            } else {
                Buffer[0] = NAK_CRC_ERROR ^ Crypto1Nibble();
                return ACK_NAK_FRAME_SIZE;
            }
        } else if (Buffer[0] == CMD_WRITE) {
            if (ISO14443ACheckCRCA(Buffer, CMD_WRITE_FRAME_SIZE)) {
                /* Write command. Store the address and prepare for the upcoming data.
                * Respond with ACK. */
                CurrentAddress = Buffer[1];
                State = STATE_WRITE;
                Buffer[0] = ACK_VALUE ^ Crypto1Nibble();
            } else {
                Buffer[0] = NAK_CRC_ERROR ^ Crypto1Nibble();
            }
            return ACK_NAK_FRAME_SIZE;
        } else if (Buffer[0] == CMD_DECREMENT) {
            if (ISO14443ACheckCRCA(Buffer, CMD_DECREMENT_FRAME_SIZE)) {
                CurrentAddress = Buffer[1];
                State = STATE_DECREMENT;
                Buffer[0] = ACK_VALUE ^ Crypto1Nibble();
            } else {
                Buffer[0] = NAK_CRC_ERROR ^ Crypto1Nibble();
            }
            return ACK_NAK_FRAME_SIZE;
        } else if (Buffer[0] == CMD_INCREMENT) {
            if (ISO14443ACheckCRCA(Buffer, CMD_DECREMENT_FRAME_SIZE)) {
                CurrentAddress = Buffer[1];
                State = STATE_INCREMENT;
                Buffer[0] = ACK_VALUE ^ Crypto1Nibble();
            } else {
                Buffer[0] = NAK_CRC_ERROR ^ Crypto1Nibble();
            }
            return ACK_NAK_FRAME_SIZE;
        } else if (Buffer[0] == CMD_RESTORE) {
            if (ISO14443ACheckCRCA(Buffer, CMD_DECREMENT_FRAME_SIZE)) {
                CurrentAddress = Buffer[1];
                State = STATE_RESTORE;
                Buffer[0] = ACK_VALUE ^ Crypto1Nibble();
            } else {
                Buffer[0] = NAK_CRC_ERROR ^ Crypto1Nibble();
            }
            return ACK_NAK_FRAME_SIZE;
        }else if (Buffer[0] == CMD_TRANSFER) {
            /* Write back the global block buffer to the desired block address */
            if (ISO14443ACheckCRCA(Buffer, CMD_TRANSFER_FRAME_SIZE)) {
                if (!ActiveConfiguration.ReadOnly) {
                    MemoryWriteBlock(BlockBuffer, (uint16_t) Buffer[1] * MEM_BYTES_PER_BLOCK, MEM_BYTES_PER_BLOCK );
                } else {
                    /* In read only mode, silently ignore the write */
                }

                Buffer[0] = ACK_VALUE ^ Crypto1Nibble();
            } else {
                Buffer[0] = NAK_CRC_ERROR ^ Crypto1Nibble();
            }

            return ACK_NAK_FRAME_SIZE;
        } else if ( (Buffer[0] == CMD_AUTH_A) || (Buffer[0] == CMD_AUTH_B) ) {
            if (ISO14443ACheckCRCA(Buffer, CMD_AUTH_FRAME_SIZE)) {
                /* Nested authentication. */
                uint8_t SectorAddress;
                uint8_t KeyOffset = (Buffer[0] == CMD_AUTH_A ? MEM_KEY_A_OFFSET : MEM_KEY_B_OFFSET);
                /* Fix for MFClassic 4k cards */
                if(Buffer[1] >= 128) {
                    SectorAddress = Buffer[1] & MEM_BIGSECTOR_ADDR_MASK;
                    KeyOffset += MEM_KEY_BIGSECTOR_OFFSET;
                } else {
                    SectorAddress = Buffer[1] & MEM_SECTOR_ADDR_MASK;
                }
                uint16_t KeyAddress = (uint16_t) SectorAddress * MEM_BYTES_PER_BLOCK + KeyOffset;
                uint8_t Key[6];
                uint8_t Uid[4];
				uint8_t CardNonce[4] = {0x01};

                /* Generate a random nonce and read UID and key from memory */
                //RandomGetBuffer(CardNonce, sizeof(CardNonce));
                if (_7BUID)
					MemoryReadBlock(Uid, MEM_UID_CL2_ADDRESS, MEM_UID_CL2_SIZE);
                else
                    MemoryReadBlock(Uid, MEM_UID_CL1_ADDRESS, MEM_UID_CL1_SIZE);

                MemoryReadBlock(Key, KeyAddress, MEM_KEY_SIZE);

                /* Precalculate the reader response from card-nonce */
                for (uint8_t i=0; i<sizeof(ReaderResponse); i++)
                    ReaderResponse[i] = CardNonce[i];

                Crypto1PRNG(ReaderResponse, 64);

                /* Precalculate our response from the reader response */
                for (uint8_t i=0; i<sizeof(CardResponse); i++)
                    CardResponse[i] = ReaderResponse[i];

                Crypto1PRNG(CardResponse, 32);

                /* Setup crypto1 cipher. */
                Crypto1Setup(Key, Uid, CardNonce);

                for (uint8_t i=0; i<sizeof(CardNonce); i++)
                    Buffer[i] = CardNonce[i];

                /* Respond with the encrypted random card nonce and expect further authentication
                * form the reader in the next frame. */
                State = STATE_AUTHING;

                return CMD_AUTH_RB_FRAME_SIZE * BITS_PER_BYTE;
            } else {
                Buffer[0] = NAK_CRC_ERROR ^ Crypto1Nibble();
                return ACK_NAK_FRAME_SIZE;
            }
        } else {
            /* Unknown command. Enter HALT state */
            State = STATE_IDLE;
        }

        break;

    case STATE_WRITE:
        /* The reader has issued a write command earlier and is now
         * sending the data to be written. Decrypt the data first and
         * check for CRC. Then write the data when ReadOnly mode is not
         * activated. */

        /* We receive 16 bytes of data to be written and 2 bytes CRCA. Decrypt */
        for (uint8_t i=0; i<(MEM_BYTES_PER_BLOCK + ISO14443A_CRCA_SIZE); i++)
            Buffer[i] ^= Crypto1Byte();

        if (ISO14443ACheckCRCA(Buffer, MEM_BYTES_PER_BLOCK)) {
            if (!ActiveConfiguration.ReadOnly) {
                MemoryWriteBlock(Buffer, CurrentAddress * MEM_BYTES_PER_BLOCK, MEM_BYTES_PER_BLOCK);
            } else {
                /* Silently ignore in ReadOnly mode */
            }

            Buffer[0] = ACK_VALUE ^ Crypto1Nibble();
        } else {
            Buffer[0] = NAK_CRC_ERROR ^ Crypto1Nibble();
        }

        State = STATE_AUTHED_IDLE;
        return ACK_NAK_FRAME_SIZE;

    case STATE_DECREMENT:
    case STATE_INCREMENT:
    case STATE_RESTORE:
        /* When we reach here, a decrement, increment or restore command has
         * been issued earlier and the reader is now sending the data. First,
         * decrypt the data and check CRC. Read data from the requested block
         * address into the global block buffer and check for integrity. Then
         * add or subtract according to issued command if necessary and store
         * the block back into the global block buffer. */
        for (uint8_t i=0; i<(MEM_VALUE_SIZE  + ISO14443A_CRCA_SIZE); i++)
            Buffer[i] ^= Crypto1Byte();

        if (ISO14443ACheckCRCA(Buffer, MEM_VALUE_SIZE )) {
            MemoryReadBlock(BlockBuffer, (uint16_t) CurrentAddress * MEM_BYTES_PER_BLOCK, MEM_BYTES_PER_BLOCK);

            if (CheckValueIntegrity(BlockBuffer)) {
                uint32_t ParamValue;
                uint32_t BlockValue;

                ValueFromBlock(&ParamValue, Buffer);
                ValueFromBlock(&BlockValue, BlockBuffer);

                if (State == STATE_DECREMENT) {
                    BlockValue -= ParamValue;
                } else if (State == STATE_INCREMENT) {
                    BlockValue += ParamValue;
                } else if (State == STATE_RESTORE) {
                    /* Do nothing */
                }

                ValueToBlock(BlockBuffer, BlockValue);

                State = STATE_AUTHED_IDLE;
                /* No ACK response on value commands part 2 */
                return ISO14443A_APP_NO_RESPONSE;
            } else {
                /* Not sure if this is the correct error code.. */
                Buffer[0] = NAK_OTHER_ERROR ^ Crypto1Nibble();
            }
        } else {
            /* CRC Error. */
            Buffer[0] = NAK_CRC_ERROR ^ Crypto1Nibble();
        }

        State = STATE_AUTHED_IDLE;
        return ACK_NAK_FRAME_SIZE;
        break;


    default:
        /* Unknown state? Should never happen. */
        break;
    }

    /* No response has been sent, when we reach here */
    return ISO14443A_APP_NO_RESPONSE;
}

void MifareClassicGetUid(ConfigurationUidType Uid)
{
	if (_7BUID) {
		//Uid[0]=0x88;
		MemoryReadBlock(&Uid[0], MEM_UID_CL1_ADDRESS, MEM_UID_CL1_SIZE-1);
		MemoryReadBlock(&Uid[3], MEM_UID_CL2_ADDRESS, MEM_UID_CL2_SIZE);
	} else {
        MemoryReadBlock(Uid, MEM_UID_CL1_ADDRESS, MEM_UID_CL1_SIZE);
	}
}

void MifareClassicSetUid(ConfigurationUidType Uid)
{
    if (_7BUID) {
	    //Uid[0]=0x88;
	    MemoryWriteBlock(Uid, MEM_UID_CL1_ADDRESS, ActiveConfiguration.UidSize);
    } else {
        uint8_t BCC =  Uid[0] ^ Uid[1] ^ Uid[2] ^ Uid[3];
		MemoryWriteBlock(Uid, MEM_UID_CL1_ADDRESS, MEM_UID_CL1_SIZE);
        MemoryWriteBlock(&BCC, MEM_UID_BCC1_ADDRESS, ISO14443A_CL_BCC_SIZE);
    }
}

void MifareClassicGetAtqa(uint16_t * Atqa)
{
	*Atqa = CardATQAValue;
}

void MifareClassicSetAtqa(uint16_t Atqa)
{
	CardATQAValue = Atqa;
}

void MifareClassicGetSak(uint8_t * Sak)
{
	*Sak = CardSAKValue;
}

void MifareClassicSetSak(uint8_t Sak)
{
	CardSAKValue = Sak;
}

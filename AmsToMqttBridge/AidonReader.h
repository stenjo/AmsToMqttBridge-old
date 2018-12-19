#ifndef _ADIONREADER_h
#define _ADIONREADER_h

#include "Crc16.h"

#if defined(ARDUINO) && ARDUINO >= 100
  #include "arduino.h"
#else
  #include "WProgram.h"
#endif

#define ADION_READER_BUFFER_SIZE 512
#define ADION_READER_MAX_ADDRESS_SIZE 5

class AidonReader
{
  public:
    AidonReader();
    bool Read(byte data);
    int GetRawData(byte *buffer, int start, int length);
    void Clear();
    
  protected:
    Crc16Class Crc16;
    
  private:
    byte buffer[ADION_READER_BUFFER_SIZE];
    int position;
    int dataLength;
    byte frameFormatType;
    byte destinationAddress[ADION_READER_MAX_ADDRESS_SIZE];
    byte destinationAddressLength;
    byte sourceAddress[ADION_READER_MAX_ADDRESS_SIZE];
    byte sourceAddressLength;
    bool FrameEscape;
    bool EscEnd;
    bool EscEsc;

    int GetAddress(int addressPosition, byte* buffer, int start, int length);
    unsigned short GetChecksum(int checksumPosition);
    bool IsValidFrameFormat(byte frameFormatType);
    void WriteBuffer();
};

#endif

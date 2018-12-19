#include "AidonReader.h"

AidonReader::AidonReader()
{
    //this->Clear();
}

void AidonReader::Clear()
{
    this->position = 0;
    this->dataLength = 0;
    this->destinationAddressLength = 0;
    this->sourceAddressLength = 0;
    this->frameFormatType = 0;
}

bool AidonReader::Read(byte data)
{
    if (position == 0 && data != 0x37)
    {
        // we haven't started yet, wait for the start flag (no need to capture any data yet)
        return false;
    }
    else
    {
        // We have completed reading of one package, so clear and be ready for the next
        //if (dataLength > 0 && position >= dataLength + 2)
        //    Clear();
        
        if (data == 0xDB)   { // Frame escape encountered
            FrameEscape = true;
            return false;
            
        }
        else if (data == 0xDC && FrameEscape) {  // Placeholder for 0xC0
            buffer[position++] = 0xC0;
            FrameEscape = false;
            return false;
        }
        else if (data == 0xDD && FrameEscape) { // Placeholder for 0xDB
            buffer[position++] = 0xDB;
            FrameEscape = false;
            return false;
        }

        FrameEscape = false;

        // Check if we're about to run into a buffer overflow
        if (position >= ADION_READER_BUFFER_SIZE)
            Clear();


        // We have started, so capture every byte
        buffer[position++] = data;
        dataLength = 90;
        // We're done, check the stop flag and signal we're done
        if (data == 0xC0 && position > 80) { // Frame end occurred
            dataLength = position;
            return true;
        }
    }

    return false;
}

bool AidonReader::IsValidFrameFormat(byte frameFormatType)
{
    return frameFormatType == 0xA0;
}

int AidonReader::GetRawData(byte *dataBuffer, int start, int length)
{

    if (dataLength > 0 && position == dataLength)
    {
        int bytesWritten = 0;
        for (int i =0; i < dataLength - 1; i++)
        {
          dataBuffer[i] = buffer[i];
          bytesWritten++;
        }
        return bytesWritten;
    }
    else
        return 0;
}

int AidonReader::GetAddress(int addressPosition, byte* addressBuffer, int start, int length)
{
    int addressBufferPos = start;
    for (int i = addressPosition; i < position; i++)
    {
        addressBuffer[addressBufferPos++] = buffer[i];
        
        // LSB=1 means this was the last address byte
        if ((buffer[i] & 0x01) == 0x01)  
            break;

        // See if we've reached last byte, try again when we've got more data
        else if (i == position - 1)
            return 0;
    }
    return addressBufferPos - start;
}

ushort AidonReader::GetChecksum(int checksumPosition)
{
    return (ushort)(buffer[checksumPosition + 2] << 8 |
        buffer[checksumPosition + 1]);
}

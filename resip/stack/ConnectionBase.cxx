#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif

#include "rutil/Logger.hxx"
#include "resip/stack/ConnectionBase.hxx"
#include "resip/stack/SipMessage.hxx"
#include "resip/stack/WsDecorator.hxx"
#include "rutil/WinLeakCheck.hxx"

#ifdef USE_SSL
#include "resip/stack/ssl/Security.hxx"
#include "resip/stack/ssl/TlsConnection.hxx"
#include "rutil/ssl/SHA1Stream.hxx"
#endif

#include "rutil/MD5Stream.hxx"

#ifdef USE_SIGCOMP
#include <osc/Stack.h>
#include <osc/TcpStream.h>
#include <osc/SigcompMessage.h>
#include <osc/StateChanges.h>
#endif

using namespace resip;

#define RESIPROCATE_SUBSYSTEM Subsystem::TRANSPORT

char 
ConnectionBase::connectionStates[ConnectionBase::MAX][32] = { "NewMessage", "ReadingHeaders", "PartialBody" };

size_t
ConnectionBase::messageSizeMax = RESIP_SIP_MSG_MAX_BYTES;

ConnectionBase::ConnectionBase(Transport* transport, const Tuple& who, Compression &compression)
   : mSendPos(0),
     mTransport(transport),
     mWho(who),
     mFailureReason(TransportFailure::None),
     mFailureSubCode(0),
     mCompression(compression),
// NO: #ifdef USE_SIGCOMP // class def doesn't decl members conditionally
     mSigcompStack(0),
     mSigcompFramer(0),
// NO: #endif
     mSendingTransmissionFormat(Unknown),
     mReceivingTransmissionFormat(Unknown),
     mDeprecatedWebSocketVersion(false),
     mMessage(0),
     mBuffer(0),
     mBufferPos(0),
     mBufferSize(0),
     mLastUsed(Timer::getTimeMs()),
     mConnState(NewMessage)
{
   DebugLog (<< "ConnectionBase::ConnectionBase, who: " << mWho << " " << this);
#ifdef USE_SIGCOMP
   if (mCompression.isEnabled())
   {
      DebugLog (<< "Compression enabled for connection: " << this);
      mSigcompStack = new osc::Stack(mCompression.getStateHandler());
      mCompression.addCompressorsToStack(mSigcompStack);
   }
   else
   {
      DebugLog (<< "Compression disabled for connection: " << this);
   }
#else
   DebugLog (<< "No compression library available: " << this);
#endif

   // deprecated; stop doing this eventually
   mWho.transport=mTransport;
   mWho.transportKey=mTransport ? mTransport->getKey() : 0;
}

ConnectionBase::~ConnectionBase()
{
   if(mTransport)
   {
      mTransport->flowTerminated(mWho);
   }

   while (!mOutstandingSends.empty())
   {
      SendData* sendData = mOutstandingSends.front();
      mTransport->fail(sendData->transactionId,
         mFailureReason ? mFailureReason : TransportFailure::ConnectionUnknown,
         mFailureSubCode);
      delete sendData;
      mOutstandingSends.pop_front();
   }
   delete [] mBuffer;
   delete mMessage;
#ifdef USE_SIGCOMP
   delete mSigcompStack;
#endif

   DebugLog (<< "ConnectionBase::~ConnectionBase " << this);
}

void
ConnectionBase::setFailureReason(TransportFailure::FailureReason failReason, int subCode)
{
   if ( failReason > mFailureReason )
   {
      mFailureReason = failReason;
      mFailureSubCode = subCode;
   }
}

FlowKey
ConnectionBase::getFlowKey() const
{
   return mWho.mFlowKey;
}

bool
ConnectionBase::preparseNewBytes(int bytesRead)
{
   DebugLog(<< "In State: " << connectionStates[mConnState]);
   
  start:   // If there is an overhang come back here, effectively recursing
   
   switch(mConnState)
   {
      case NewMessage:
      {
         if (strncmp(mBuffer + mBufferPos, Symbols::CRLFCRLF, 4) == 0)
         {
            DebugLog(<< "Got incoming double-CRLF keepalive (aka ping).");
            mBufferPos += 4;
            bytesRead -= 4;
            onDoubleCRLF();
            if (bytesRead)
            {
               goto start;
            }
            else
            {
               delete [] mBuffer;
               mBuffer = 0;
               return true;
            }
         }
         else if (strncmp(mBuffer + mBufferPos, Symbols::CRLF, 2) == 0)
         {
            //DebugLog(<< "Got incoming CRLF keepalive response (aka pong).");
            mBufferPos += 2;
            bytesRead -= 2;
            onSingleCRLF();
            if (bytesRead)
            {
               goto start;
            }
            else
            {
               delete [] mBuffer;
               mBuffer = 0;
               return true;
            }
         }

         assert(mTransport);
         mMessage = new SipMessage(mTransport);
         
         DebugLog(<< "ConnectionBase::process setting source " << mWho);
         mMessage->setSource(mWho);
         mMessage->setTlsDomain(mTransport->tlsDomain());

#ifdef USE_SSL
         // Set TlsPeerName if message is from TlsConnection
         TlsConnection *tlsConnection = dynamic_cast<TlsConnection *>(this);
         if(tlsConnection)
         {
            std::list<Data> peerNameList;
            tlsConnection->getPeerNames(peerNameList);
            mMessage->setTlsPeerNames(peerNameList);
         }
#endif
         mMsgHeaderScanner.prepareForMessage(mMessage);
         // Fall through to the next case.
      }
      case ReadingHeaders:
      {
         unsigned int chunkLength = (unsigned int)mBufferPos + bytesRead;
         char *unprocessedCharPtr;
         MsgHeaderScanner::ScanChunkResult scanChunkResult =
            mMsgHeaderScanner.scanChunk(mBuffer,
                                        chunkLength,
                                        &unprocessedCharPtr);
         if (scanChunkResult == MsgHeaderScanner::scrError)
         {
            //.jacob. Not a terribly informative warning.
            WarningLog(<< "Discarding preparse!");
            delete [] mBuffer;
            mBuffer = 0;
            delete mMessage;
            mMessage = 0;
            mConnState=NewMessage;
            return false;
         }

         if (mMsgHeaderScanner.getHeaderCount() > 256)
         {
            WarningLog(<< "Discarding preparse; too many headers");
            delete [] mBuffer;
            mBuffer = 0;
            delete mMessage;
            mMessage = 0;
            mConnState=NewMessage;
            return false;
         }

         unsigned int numUnprocessedChars = 
            (unsigned int)((mBuffer + chunkLength) - unprocessedCharPtr);

         if(numUnprocessedChars > ConnectionBase::ChunkSize &&
            scanChunkResult == MsgHeaderScanner::scrNextChunk)
         {
            WarningLog(<< "Discarding preparse; header-field-value (or "
                        "header name) too long");
            delete [] mBuffer;
            mBuffer = 0;
            delete mMessage;
            mMessage = 0;
            mConnState=NewMessage;
            return false;
         }

         if(numUnprocessedChars==chunkLength)
         {
            // .bwc. MsgHeaderScanner wasn't able to parse anything useful;
            // don't bother mMessage yet, but make more room in mBuffer.
            size_t size = numUnprocessedChars*3/2;
            if (size < ConnectionBase::ChunkSize)
            {
               size = ConnectionBase::ChunkSize;
            }
            char* newBuffer = 0;
            try
            {
               newBuffer=MsgHeaderScanner::allocateBuffer((int)size);
            }
            catch(std::bad_alloc&)
            {
               ErrLog(<<"Failed to alloc a buffer during preparse!");
               return false;
            }
            memcpy(newBuffer, unprocessedCharPtr, numUnprocessedChars);
            delete [] mBuffer;
            mBuffer = newBuffer;
            mBufferPos = numUnprocessedChars;
            mBufferSize = size;
            mConnState = ReadingHeaders;
            return true;
         }

         mMessage->addBuffer(mBuffer);
         mBuffer=0;

         if (scanChunkResult == MsgHeaderScanner::scrNextChunk)
         {
            // Message header is incomplete...
            if (numUnprocessedChars == 0)
            {
               // ...but the chunk is completely processed.
               //.jacob. I've discarded the "assigned" concept.
               //DebugLog(<< "Data assigned, not fragmented, not complete");
               try
               {
                  mBuffer = MsgHeaderScanner::allocateBuffer(ChunkSize);
               }
               catch(std::bad_alloc&)
               {
                  ErrLog(<<"Failed to alloc a buffer during preparse!");
                  return false;
               }
               mBufferPos = 0;
               mBufferSize = ChunkSize;
            }
            else
            {
               // ...but some of the chunk must be shifted into the next one.
               size_t size = numUnprocessedChars*3/2;
               if (size < ConnectionBase::ChunkSize)
               {
                  size = ConnectionBase::ChunkSize;
               }
               char* newBuffer = 0;
               try
               {
                  newBuffer = MsgHeaderScanner::allocateBuffer((int)size);
               }
               catch(std::bad_alloc&)
               {
                  ErrLog(<<"Failed to alloc a buffer during preparse!");
                  return false;
               }
               memcpy(newBuffer, unprocessedCharPtr, numUnprocessedChars);
               mBuffer = newBuffer;
               mBufferPos = numUnprocessedChars;
               mBufferSize = size;
            }
            mConnState = ReadingHeaders;
         }
         else
         {
            size_t contentLength = 0;
            
            try
            {
               // The message header is complete.
               contentLength=mMessage->const_header(h_ContentLength).value();
            }
            catch(resip::BaseException& e)  // Could be SipMessage::Exception or ParseException
            {
               WarningLog(<<"Malformed Content-Length in connection-based transport"
                           ". Not much we can do to fix this.  " << e);
               // .bwc. Bad Content-Length. We are hosed.
               delete mMessage;
               mMessage = 0;
               mBuffer = 0;
               // .bwc. mMessage just took ownership of mBuffer, so we don't
               // delete it here. We do zero it though, for completeness.
               //.jacob. Shouldn't the state also be set here?
               return false;
            }
            
            if(contentLength > messageSizeMax || contentLength < 0)
            {
               WarningLog(<<"Content-Length in connection-based "
                           "transport exceeds maximum " << messageSizeMax);
               delete mMessage;
               mMessage = 0;
               mBuffer = 0;
               // .bwc. mMessage just took ownership of mBuffer, so we don't
               // delete it here. We do zero it though, for completeness.
               //.jacob. Shouldn't the state also be set here?
               return false;
            }

            if (numUnprocessedChars < contentLength)
            {
               // The message body is incomplete.
               DebugLog(<< "partial body received");
               size_t newSize=resipMin(resipMax((size_t)numUnprocessedChars*3/2,
                                             (size_t)ConnectionBase::ChunkSize),
                                    contentLength);
               char* newBuffer = MsgHeaderScanner::allocateBuffer((int)newSize);
               memcpy(newBuffer, unprocessedCharPtr, numUnprocessedChars);
               mBufferPos = numUnprocessedChars;
               mBufferSize = newSize;
               mBuffer = newBuffer;
               
               mConnState = PartialBody;
            }
            else
            {
               // Do this stuff BEFORE we kick the message out the door.
               // Remember, deleting or passing mMessage on invalidates our
               // buffer!
               int overHang = numUnprocessedChars - (int)contentLength;

               mConnState = NewMessage;
               mBuffer = 0;
               if (overHang > 0) 
               {
                  // The next message has been partially read.
                  size_t size = overHang*3/2;
                  if (size < ConnectionBase::ChunkSize)
                  {
                     size = ConnectionBase::ChunkSize;
                  }
                  char* newBuffer = MsgHeaderScanner::allocateBuffer((int)size);
                  memcpy(newBuffer,
                         unprocessedCharPtr + contentLength,
                         overHang);
                  mBuffer = newBuffer;
                  mBufferPos = 0;
                  mBufferSize = size;
                  
                  DebugLog (<< "Extra bytes after message: " << overHang);
                  DebugLog (<< Data(mBuffer, overHang));
                  
                  bytesRead = overHang;
               }

               // The message body is complete.
               mMessage->setBody(unprocessedCharPtr, (UInt32)contentLength);
               CongestionManager::RejectionBehavior b=mTransport->getRejectionBehaviorForIncoming();
               if (b==CongestionManager::REJECTING_NON_ESSENTIAL
                     || (b==CongestionManager::REJECTING_NEW_WORK
                        && mMessage->isRequest()))
               {
                  UInt32 expectedWait(mTransport->getExpectedWaitForIncoming());
                  // .bwc. If this fifo is REJECTING_NEW_WORK, we will drop
                  // requests but not responses ( ?bwc? is this right for ACK?). 
                  // If we are REJECTING_NON_ESSENTIAL, 
                  // we reject all incoming work, since losing something from the 
                  // wire will not cause instability or leaks (see 
                  // CongestionManager.hxx)
                  
                  // .bwc. This handles all appropriate checking for whether
                  // this is a response or an ACK.
                  std::auto_ptr<SendData> tryLater(transport()->make503(*mMessage, expectedWait/1000));
                  if(tryLater.get())
                  {
                     transport()->send(tryLater);
                  }
                  delete mMessage; // dropping message due to congestion
                  mMessage = 0;
               }
               else if (!transport()->basicCheck(*mMessage))
               {
                  delete mMessage;
                  mMessage = 0;
               }
               else
               {
                  Transport::stampReceived(mMessage);
                  DebugLog(<< "##Connection: " << *this << " received: " << *mMessage);
                  assert( mTransport );
                  mTransport->pushRxMsgUp(mMessage);
                  mMessage = 0;
               }

               if (overHang > 0) 
               {
                  goto start;
               }
            }
         }
         break;
      }
      case PartialBody:
      {
         size_t contentLength = 0;

         try
         {
             contentLength = mMessage->const_header(h_ContentLength).value();
         }
         catch(resip::BaseException& e)  // Could be SipMessage::Exception or ParseException
         {
            WarningLog(<<"Malformed Content-Length in connection-based transport"
                        ". Not much we can do to fix this. " << e);
            // .bwc. Bad Content-Length. We are hosed.
            delete [] mBuffer;
            mBuffer = 0;
            delete mMessage;
            mMessage = 0;
            //.jacob. Shouldn't the state also be set here?
            return false;
         }

         mBufferPos += bytesRead;
         if (mBufferPos == contentLength)
         {
            mMessage->addBuffer(mBuffer);
            mMessage->setBody(mBuffer, (UInt32)contentLength);
            mBuffer=0;
            // .bwc. basicCheck takes up substantial CPU. Don't bother doing it
            // if we're overloaded.
            CongestionManager::RejectionBehavior b=mTransport->getRejectionBehaviorForIncoming();
            if (b==CongestionManager::REJECTING_NON_ESSENTIAL
                  || (b==CongestionManager::REJECTING_NEW_WORK
                     && mMessage->isRequest()))
            {
               UInt32 expectedWait(mTransport->getExpectedWaitForIncoming());
               // .bwc. If this fifo is REJECTING_NEW_WORK, we will drop
               // requests but not responses ( ?bwc? is this right for ACK?). 
               // If we are REJECTING_NON_ESSENTIAL, 
               // we reject all incoming work, since losing something from the 
               // wire will not cause instability or leaks (see 
               // CongestionManager.hxx)
               
               // .bwc. This handles all appropriate checking for whether
               // this is a response or an ACK.
               std::auto_ptr<SendData> tryLater = transport()->make503(*mMessage, expectedWait/1000);
               if(tryLater.get())
               {
                  transport()->send(tryLater);
               }
               delete mMessage; // dropping message due to congestion
               mMessage = 0;
            }
            else if (!transport()->basicCheck(*mMessage))
            {
               delete mMessage;
               mMessage = 0;
            }
            else
            {
               DebugLog(<< "##ConnectionBase: " << *this << " received: " << *mMessage);

               Transport::stampReceived(mMessage);
               assert( mTransport );
               mTransport->pushRxMsgUp(mMessage);
               mMessage = 0;
            }
            mConnState = NewMessage;
         }
         else if (mBufferPos == mBufferSize)
         {
            // .bwc. We've filled our buffer; go ahead and make more room.
            size_t newSize = resipMin(mBufferSize*3/2, contentLength);
            char* newBuffer = 0;
            try
            {
               newBuffer=new char[newSize];
            }
            catch(std::bad_alloc&)
            {
               ErrLog(<<"Failed to alloc a buffer while receiving body!");
               return false;
            }
            memcpy(newBuffer, mBuffer, mBufferSize);
            mBufferSize=newSize;
            delete [] mBuffer;
            mBuffer = newBuffer;
         }
         break;
      }
      default:
         assert(0);
   }
   return true;
}

/*
 * Returns true if handshake complete, false if more bytes needed
 * Sets dropConnection = true if an error occurs
 */
bool
ConnectionBase::wsProcessHandshake(int bytesRead, bool &dropConnection)
{
   mConnState = WebSocket;
   dropConnection = false;

   if(mBufferPos + bytesRead > messageSizeMax)
   {
      WarningLog(<<"Too many bytes received during WS handshake, dropping connection.  Max message size = " << messageSizeMax);
      dropConnection = true;
      return false;
   }

   mMessage = new SipMessage(mWho.transport);
   assert(mMessage);

   mMessage->setSource(mWho);
   mMessage->setTlsDomain(mWho.transport->tlsDomain());

   mMsgHeaderScanner.prepareForMessage(mMessage);
   char *unprocessedCharPtr;
   MsgHeaderScanner::ScanChunkResult scanResult = mMsgHeaderScanner.scanChunk(mBuffer, mBufferPos + bytesRead, &unprocessedCharPtr);
   if (scanResult != MsgHeaderScanner::scrEnd)
   {
      if(scanResult != MsgHeaderScanner::scrNextChunk){
         StackLog(<<"Failed to parse message, more bytes needed");
         StackLog(<< Data(mBuffer, bytesRead));
      }
      delete mMessage;
      mMessage=0;
      mBufferPos += bytesRead;
      return false;
   }

   try{
      Data wsResponse;
      wsResponse =		"HTTP/1.1 101 WebSocket Protocol Handshake\r\n"
            "Upgrade: WebSocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Protocol: sip\r\n";

      if(mMessage->exists(h_SecWebSocketKey1) && mMessage->exists(h_SecWebSocketKey2)){
         Data SecWebSocketKey1 =  mMessage->const_header(h_SecWebSocketKey1).value();
         Data SecWebSocketKey2 =  mMessage->const_header(h_SecWebSocketKey2).value();
         Data Digits1, Digits2;
         unsigned int SpacesCount1 = 0, SpacesCount2 = 0;
         unsigned int Value1, Value2;
         for(unsigned int i = 0; i < SecWebSocketKey1.size(); ++i){
            if(SecWebSocketKey1[i] == ' ') ++SpacesCount1;
            if(isdigit(SecWebSocketKey1[i])) Digits1 += SecWebSocketKey1[i];
         }
         Value1 = htonl(Digits1.convertUnsignedLong() / SpacesCount1);
         for(unsigned int i = 0; i < SecWebSocketKey2.size(); ++i){
            if(SecWebSocketKey2[i] == ' ') ++SpacesCount2;
            if(isdigit(SecWebSocketKey2[i])) Digits2 += SecWebSocketKey2[i];
         }
         Value2 = htonl(Digits2.convertUnsignedLong() / SpacesCount2);

         MD5Stream wsMD5Stream;
         char tmp[9] = { '\0' };
         memcpy(tmp, &Value1, 4);
         memcpy(&tmp[4], &Value2, 4);
         wsMD5Stream << tmp;
         if(unprocessedCharPtr < (mBuffer + mBufferPos + bytesRead)){
            unsigned int dataLen = (mBuffer + mBufferPos + bytesRead) - unprocessedCharPtr;
            Data content(unprocessedCharPtr, dataLen);
            wsMD5Stream << content;
         }

         if(mMessage->exists(h_Origin)){
            wsResponse += "Sec-WebSocket-Origin: " + mMessage->const_header(h_Origin).value() + "\r\n";
         }
         if(mMessage->exists(h_Host)){
            wsResponse += Data("Sec-WebSocket-Location: ") + Data(transport()->transport() == resip::WSS ? "wss://" : "ws://") + mMessage->const_header(h_Host).value() + Data("/\r\n");
         }
         wsResponse += "\r\n" + wsMD5Stream.getBin();
         mDeprecatedWebSocketVersion = true;
      }
      else if(mMessage->exists(h_SecWebSocketKey)){
         SHA1Stream wsSha1Stream;
         wsSha1Stream << (mMessage->const_header(h_SecWebSocketKey).value() + Data("258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
         Data wsAcceptKey = wsSha1Stream.getBin(160).base64encode();
         wsResponse +=	"Sec-WebSocket-Accept: "+ wsAcceptKey +"\r\n"
               "\r\n";
         mDeprecatedWebSocketVersion = false;
      }
      else{
         ErrLog(<<"No SecWebSocketKey header");
         delete mMessage;
         mMessage = 0;
         mBufferPos = 0;
         dropConnection = true;
         return false;
      }

      mOutstandingSends.push_back(new SendData(
            who(),
            wsResponse,
            Data::Empty,
            Data::Empty,
            true));
   }
   catch(resip::ParseException& e)
   {
      ErrLog(<<"Cannot auth request is missing " << e);
      delete mMessage;
      mMessage = 0;
      mBufferPos = 0;
      dropConnection = true;
      return false;
   }

   delete mMessage;
   mMessage=0;
   mBufferPos = 0;

   return true;
}

bool
ConnectionBase::wsProcessData(int bytesRead, bool &tryAgain)
{
   UInt8* uBuffer = (UInt8*)mBuffer;
   tryAgain = false;

   size_t bytes_available = mBufferPos + bytesRead;

   if(bytes_available > messageSizeMax)
   {
      WarningLog(<<"Too many bytes received during WS frame assembly, dropping connection.  Max message size = " << messageSizeMax);
      tryAgain = false;
      return false;
   }

   if(mDeprecatedWebSocketVersion)
   {
      ErrLog(<<"mDeprecatedWebSocketVersion not supported!");
      return false;
/*      int i, j;
      if(mBufferPos == 0)
      {
         mConnState = NewMessage;

         // http://tools.ietf.org/html/draft-hixie-thewebsocketprotocol-76#page-19 5.3.  Data framing
         const UInt8 type = mBuffer[0];
         if(!(type & 0x80))
         {
            if(type != 0x00)
            {
               ErrLog(<< "Unexpected type: ");
               return false;
            }
            for(i = j = 1; i < bytesRead; ++i)
            {
               if(uBuffer[i] == 0xFF)
               {
                  gotCompleteMsg = true;
                  break;
               }
            }
         }
         else
         {
            ErrLog(<< "Not implemented yet: ");
            return false;
         }
      }
      else
      {
         for(i = j = 0; i < bytesRead; ++i)
         {
            if(uBuffer[i] == 0xFF)
            {
               gotCompleteMsg = true;
               break;
            }
         }
      }

      bytesRead = (i - j);
      memmove(uBuffer, &uBuffer[j], (i - j));
      return true; */
   }
   else
   {
      if(bytes_available < 2)
      {
         StackLog(<< "Too short to contain ws data [0]");
         mBufferPos += bytesRead;
         return true;
      }
      UInt64 wsFrameHdrLen = 0;

      const UInt8 finalFrame = (uBuffer[0] >> 7);
      const UInt8 maskFlag = (uBuffer[1] >> 7);

      if(uBuffer[0] & 0x40 || uBuffer[0] & 0x20 || uBuffer[0] & 0x10)
      {
         WarningLog(<< "Unknown extension: " << ((uBuffer[0] >> 4) & 0x07));
         // do not exit
      }

      UInt64 wsPayLen = uBuffer[1] & 0x7F;
      wsFrameHdrLen += 2;

      if(wsPayLen == 126)
      {
         if((bytes_available - wsFrameHdrLen) < 2)
         {
            StackLog(<< "Too short to contain ws data [1]");
            mBufferPos += bytesRead;
            return true;
         }
         wsPayLen = (uBuffer[wsFrameHdrLen] << 8 | uBuffer[wsFrameHdrLen + 1]);
         wsFrameHdrLen += 2;
      }
      else if(wsPayLen == 127)
      {
         if((bytes_available - wsFrameHdrLen) < 4)
         {
            StackLog(<< "Too short to contain ws data [2]");
            mBufferPos += bytesRead;
            return true;
         }
         wsPayLen = (((UInt64)uBuffer[wsFrameHdrLen]) << 56 | ((UInt64)uBuffer[wsFrameHdrLen + 1]) << 48 | ((UInt64)uBuffer[wsFrameHdrLen + 2]) << 40 | ((UInt64)uBuffer[wsFrameHdrLen + 3]) << 32 | ((UInt64)uBuffer[wsFrameHdrLen + 4]) << 24 | ((UInt64)uBuffer[wsFrameHdrLen + 5]) << 16 | ((UInt64)uBuffer[wsFrameHdrLen + 6]) << 8 || ((UInt64)uBuffer[wsFrameHdrLen + 7]));
         wsFrameHdrLen += 8;
      }

      if((bytes_available - wsFrameHdrLen) < 4)
      {
         StackLog(<< "Too short to contain ws data [3]");
         mBufferPos += bytesRead;
         return true;
      }
      if(maskFlag)
      {
         mWsMaskKey[0] = uBuffer[wsFrameHdrLen];
         mWsMaskKey[1] = uBuffer[wsFrameHdrLen + 1];
         mWsMaskKey[2] = uBuffer[wsFrameHdrLen + 2];
         mWsMaskKey[3] = uBuffer[wsFrameHdrLen + 3];
         wsFrameHdrLen += 4;
      }

      // Validation of header fields
      if(wsPayLen > messageSizeMax)
      {
         WarningLog(<<"WS frame header describes a payload size bigger than messageSizeMax, max = " << messageSizeMax 
                 << ", dropping connection");
         tryAgain = false;
         return false;
      }

      UInt64 payIdx, frameLen;
      frameLen = wsFrameHdrLen + wsPayLen;
      if(bytes_available < frameLen)
      {
         StackLog(<< "need more bytes for a complete WS frame");
         mBufferPos += bytesRead;
         if(bytes_available == mBufferSize)
         {
            // If we get here, we're out of luck.  The size of mBuffer needs
            // to grow to handle larger WS frames.  On the other hand,
            // it should not be able to grow to consume all memory.
            ErrLog(<< "no more space in mBuffer to receive more bytes, review ChunkSize");
            return false;
         }
         return true;
      }
      StackLog(<<"Buffer has "<<bytes_available<<" bytes, frame has "<<frameLen<< " bytes, header = "<< wsFrameHdrLen<<" bytes, payload = "<< wsPayLen<<" bytes");
      UInt8 *uData = new UInt8[wsPayLen];
      for(payIdx = 0; payIdx < wsPayLen; ++payIdx)
      {
         uData[payIdx] = (uBuffer[wsFrameHdrLen+payIdx] ^ mWsMaskKey[(payIdx & 3)]);
      }
      mWsBuffer.append((char *)uData, wsPayLen);
      delete [] uData;

      // Are there more bytes available from the transport?  Leave them
      // for next iteration
      if(bytes_available > frameLen)
      {
         StackLog(<<"More bytes left over, total bytes_available = " << bytes_available << ", consumed only " << frameLen);
         for(payIdx = 0; payIdx < (bytes_available - frameLen); payIdx++)
         {
            uBuffer[payIdx] = uBuffer[frameLen+payIdx];
         }
         tryAgain = true;
      }
      mBufferPos = bytes_available - frameLen;
      StackLog(<<"new mBufferPos = "<<mBufferPos);

      if(finalFrame == 1)
      {
         // mWsBuffer should now contain a discrete SIP message, let the
         // stack go to work on it

         mMessage = new SipMessage(mWho.transport);

         mMessage->setSource(mWho);
         mMessage->setTlsDomain(mWho.transport->tlsDomain());

         Data::size_type msg_len = mWsBuffer.size();
         char *sipBuffer = new char[msg_len];
         memmove(sipBuffer, mWsBuffer.c_str(), msg_len);
         mWsBuffer.clear();
         mMessage->addBuffer(sipBuffer);
         mMsgHeaderScanner.prepareForMessage(mMessage);
         char *unprocessedCharPtr;
         if (mMsgHeaderScanner.scanChunk(sipBuffer,
                                    msg_len,
                                    &unprocessedCharPtr) !=
                       MsgHeaderScanner::scrEnd)
         {
            StackLog(<<"Scanner rejecting WebSocket SIP message as unparsable, length = " << msg_len);
            StackLog(<< Data(sipBuffer, msg_len));
            delete mMessage;
            mMessage=0;
         }

         unsigned int used = unprocessedCharPtr - sipBuffer;
         if (mMessage && (used < msg_len))
         {
            mMessage->setBody(sipBuffer+used, msg_len-used);
         }

         if (mMessage && !transport()->basicCheck(*mMessage))
         {
            delete mMessage;
            mMessage = 0;
         }

         if (mMessage)
         {
            Transport::stampReceived(mMessage);
            assert( mTransport );
            mTransport->pushRxMsgUp(mMessage);
            mMessage = 0;
         }
         else
         {
            // Something wrong...
            ErrLog(<< "We don't have a valid SIP message, maybe drop the connection?");
         }
      }
      return true;
   }
}

#ifdef USE_SIGCOMP
void
ConnectionBase::decompressNewBytes(int bytesRead)
{
  mConnState = SigComp;

  if (!mSigcompFramer)
  {
    mSigcompFramer = new osc::TcpStream();
  }

  mSigcompFramer->addData(mBuffer, bytesRead);
  size_t bytesUncompressed;
  osc::StateChanges *sc = 0;
  char *uncompressed = new char[65536];
  while ((bytesUncompressed = mSigcompStack->uncompressMessage(
                *mSigcompFramer, uncompressed, 65536, sc)) > 0)
  {
    DebugLog (<< "Uncompressed Connection-oriented message");
    mMessage = new SipMessage(mWho.transport);

    mMessage->setSource(mWho);
    mMessage->setTlsDomain(mWho.transport->tlsDomain());

    char *sipBuffer = new char[bytesUncompressed];
    memmove(sipBuffer, uncompressed, bytesUncompressed);
    mMessage->addBuffer(sipBuffer);
    mMsgHeaderScanner.prepareForMessage(mMessage);
    char *unprocessedCharPtr;
    if (mMsgHeaderScanner.scanChunk(sipBuffer,
                                    bytesUncompressed,
                                    &unprocessedCharPtr) !=
        MsgHeaderScanner::scrEnd)
    {
       StackLog(<<"Scanner rejecting compressed message as unparsable");
       StackLog(<< Data(sipBuffer, bytesUncompressed));
       delete mMessage;
       mMessage=0;
    }
  
    unsigned int used = unprocessedCharPtr - sipBuffer;
    if (mMessage && (used < bytesUncompressed))
    {
      mMessage->setBody(sipBuffer+used, bytesUncompressed-used);
    }

    if (mMessage && !transport()->basicCheck(*mMessage))
    {
      delete mMessage;
      mMessage = 0;
    }

    if (mMessage)
    {
      Transport::stampReceived(mMessage);
      // If the message made it this far, we should let it store
      // SigComp state: extract the compartment ID.
      const Via &via = mMessage->const_header(h_Vias).front();
      if (mMessage->isRequest())
      {
        // For requests, the compartment ID is read out of the
        // top via header field; if not present, we use the
        // TCP connection for identification purposes.
        if (via.exists(p_sigcompId))
        {
            Data compId = via.param(p_sigcompId);
            if(!compId.empty())
            {
                mSigcompStack->provideCompartmentId(sc, compId.data(), compId.size());
            }
        }
        else
        {
          mSigcompStack->provideCompartmentId(sc, this, sizeof(this));
        }
      }
      else
      {
        // For responses, the compartment ID is supposed to be
        // the same as the compartment ID of the request. We
        // *could* dig down into the transaction layer to try to
        // figure this out, but that's a royal pain, and a rather
        // severe layer violation. In practice, we're going to ferret
        // the ID out of the the Via header field, which is where we
        // squirreled it away when we sent this request in the first place.
        Data compId = via.param(p_branch).getSigcompCompartment();
        if(!compId.empty())
        {
           mSigcompStack->provideCompartmentId(sc, compId.data(), compId.size());
        }
      }
      assert( mTransport );
      mTransport->pushRxMsgUp(mMessage);
      mMessage = 0;
      sc = 0;
    }
    else
    {
      delete sc;
      sc = 0;
    }
  }
  delete [] uncompressed;

  // If there was a decompression failure, let the other side know.
  osc::SigcompMessage *nack = mSigcompStack->getNack();
  if (nack)
  {
    if (mSendingTransmissionFormat == Compressed)
    {
      // !bwc! We are not telling anyone that we're interested in having our
      // FD put in the writable set...
      mOutstandingSends.push_back(new SendData(
                   who(),
                   Data(nack->getStreamMessage(), nack->getStreamLength()),
                   Data::Empty,
                   Data::Empty,
                   true));
    }
    else
    {
      delete nack;
    }
  }
}
#endif
            
std::pair<char*, size_t> 
ConnectionBase::getWriteBuffer()
{
   if (mConnState == NewMessage)
   {
      if (!mBuffer)
      {
         DebugLog (<< "Creating buffer for " << *this);

         mBuffer = MsgHeaderScanner::allocateBuffer(ConnectionBase::ChunkSize);
         mBufferSize = ConnectionBase::ChunkSize;
      }
      mBufferPos = 0;
   }
   return getCurrentWriteBuffer();
}

std::pair<char*, size_t> 
ConnectionBase::getCurrentWriteBuffer()
{
   return std::make_pair(mBuffer + mBufferPos, mBufferSize - mBufferPos);
}

char*
ConnectionBase::getWriteBufferForExtraBytes(int extraBytes)
{
   if (extraBytes > 0)
   {
      char* buffer = MsgHeaderScanner::allocateBuffer((int)mBufferSize + extraBytes);
      memcpy(buffer, mBuffer, mBufferSize);
      delete [] mBuffer;
      mBuffer = buffer;
      buffer += mBufferSize;
      mBufferSize += extraBytes;
      return buffer;
   }
   else
   {
      assert(0);
      return mBuffer;
   }
}
            
void 
ConnectionBase::setBuffer(char* bytes, int count)
{
   mBuffer = bytes;
   mBufferPos = 0;
   mBufferSize = count;
}

Transport* 
ConnectionBase::transport() const
{
   assert(this);
   return mTransport;
}

EncodeStream& 
resip::operator<<(EncodeStream& strm, 
                  const resip::ConnectionBase& c)

{
   strm << "CONN_BASE: " << &c << " " << c.mWho;
   return strm;
}

/* ====================================================================
 * The Vovida Software License, Version 1.0 
 * 
 * Copyright (c) 2000
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 
 * 3. The names "VOCAL", "Vovida Open Communication Application Library",
 *    and "Vovida Open Communication Application Library (VOCAL)" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact vocal@vovida.org.
 *
 * 4. Products derived from this software may not be called "VOCAL", nor
 *    may "VOCAL" appear in their name, without prior written
 *    permission of Vovida Networks, Inc.
 * 
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL VOVIDA
 * NETWORKS, INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT DAMAGES
 * IN EXCESS OF $1,000, NOR FOR ANY INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 * 
 * ====================================================================
 * 
 * This software consists of voluntary contributions made by Vovida
 * Networks, Inc. and many individuals on behalf of Vovida Networks,
 * Inc.  For more information on Vovida Networks, Inc., please see
 * <http://www.vovida.org/>.
 *
 * vi: set shiftwidth=3 expandtab:
 */


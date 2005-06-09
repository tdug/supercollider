/*
	SuperCollider real time audio synthesis system
    Copyright (c) 2002 James McCartney. All rights reserved.
	http://www.audiosynth.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "OSCData.h"
#include "PyrPrimitive.h"
#include "PyrKernel.h"
#include "PyrInterpreter.h"
#include "PyrSched.h"
#include "GC.h"
//#include "PyrOMS.h"
//#include "MidiQ.h"
#include <string.h>
#include <math.h>
#include <stdexcept>
#include <new.h>

#ifdef SC_WIN32
# include <winsock2.h>
typedef int socklen_t;
# define bzero( ptr, count ) memset( ptr, 0, count )
#else
# include <sys/socket.h>
# include <netinet/tcp.h>
# include <netdb.h>
#endif

#include <pthread.h>
#include "scsynthsend.h"
#include "sc_msg_iter.h"
#include "SC_ComPort.h"
#include "SC_WorldOptions.h"
#include "SC_SndBuf.h"
#include "SC_Endian.h"

#ifndef SC_DARWIN
# ifndef SC_WIN32
#  include <unistd.h>
# endif
#endif

struct InternalSynthServerGlobals
{
	struct World *mWorld;
	int mNumSharedControls;
	float *mSharedControls;
};

const int kNumDefaultSharedControls = 1024;
float gDefaultSharedControls[kNumDefaultSharedControls];
bool gUseDoubles = false;

InternalSynthServerGlobals gInternalSynthServer = { 0, kNumDefaultSharedControls, gDefaultSharedControls };

SC_UdpInPort* gUDPport = 0;

PyrString* newPyrString(VMGlobals *g, char *s, int flags, bool collect);

PyrSymbol *s_call, *s_write, *s_recvoscmsg, *s_recvoscbndl, *s_netaddr;
const char* gPassword;
extern bool compiledOK;

#define USE_SCHEDULER 1


///////////

inline bool IsBundle(char* ptr) 
{ 
	return strcmp(ptr, "#bundle") == 0; 
}

///////////

scpacket gSynthPacket;

const int ivxNetAddr_Hostaddr = 0;
const int ivxNetAddr_PortID = 1;
const int ivxNetAddr_Hostname = 2;
const int ivxNetAddr_Socket = 3;

void makeSockAddr(struct sockaddr_in &toaddr, int32 addr, int32 port);
int sendallto(int socket, const void *msg, size_t len, struct sockaddr *toaddr, int addrlen);
int sendall(int socket, const void *msg, size_t len);
int makeSynthMsgWithTags(scpacket *packet, PyrSlot *slots, int size);
int makeSynthBundle(scpacket *packet, PyrSlot *slots, int size, bool useElapsed);

void addMsgSlot(scpacket *packet, PyrSlot *slot);
void addMsgSlot(scpacket *packet, PyrSlot *slot)
{
	switch (slot->utag) {
		case tagInt :
			packet->addi(slot->ui);
			break;
		case tagSym :
			packet->adds(slot->us->name);
			break;
		case tagObj :
			if (isKindOf(slot->uo, class_string)) {
				PyrString *stringObj = slot->uos;
				packet->adds(stringObj->s, stringObj->size);
			} else if (isKindOf(slot->uo, class_int8array)) {
				PyrInt8Array *arrayObj = slot->uob;
				packet->addb(arrayObj->b, arrayObj->size);
			} else if (isKindOf(slot->uo, class_array)) {
				PyrObject *arrayObj = slot->uo;
				scpacket packet2;
				if (arrayObj->size > 1 && isKindOfSlot(arrayObj->slots+1, class_array)) {
					makeSynthBundle(&packet2, arrayObj->slots, arrayObj->size, true);
				} else {
					makeSynthMsgWithTags(&packet2, arrayObj->slots, arrayObj->size);
				}
				packet->addb((uint8*)packet2.data(), packet2.size());
			}
			break;
		case tagNil :
		case tagTrue :
		case tagFalse :
		case tagChar :
		case tagPtr :
			break;
		default :
			if (gUseDoubles) packet->addd(slot->uf);
			else packet->addf(slot->uf);
			break;
	}
}

void addMsgSlotWithTags(scpacket *packet, PyrSlot *slot);
void addMsgSlotWithTags(scpacket *packet, PyrSlot *slot)
{
	switch (slot->utag) {
		case tagInt :
			packet->addtag('i');
			packet->addi(slot->ui);
			break;
		case tagSym :
			packet->addtag('s');
			packet->adds(slot->us->name);
			break;
		case tagObj :
			if (isKindOf(slot->uo, class_string)) {
				PyrString *stringObj = slot->uos;
				packet->addtag('s');
				packet->adds(stringObj->s, stringObj->size);
			} else if (isKindOf(slot->uo, class_int8array)) {
				PyrInt8Array *arrayObj = slot->uob;
				packet->addtag('b');
				packet->addb(arrayObj->b, arrayObj->size);
			} else if (isKindOf(slot->uo, class_array)) {
				PyrObject *arrayObj = slot->uo;
				if (arrayObj->size) {
					packet->addtag('b');
					scpacket packet2;
					if (arrayObj->size > 1 && isKindOfSlot(arrayObj->slots+1, class_array)) {
						makeSynthBundle(&packet2, arrayObj->slots, arrayObj->size, true);
					} else {
						makeSynthMsgWithTags(&packet2, arrayObj->slots, arrayObj->size);
					}
					packet->addb((uint8*)packet2.data(), packet2.size());
				} else {
					packet->addtag('i');
					packet->addi(0);
				}
			}
			break;
		case tagTrue :
			packet->addtag('i');
			packet->addi(1);
			break;
		case tagFalse :
		case tagNil :
		case tagChar :
		case tagPtr :
			packet->addtag('i');
			packet->addi(0);
			break;
		default :
			if (gUseDoubles) {
				packet->addtag('d');
				packet->addd(slot->uf);
			} else {
				packet->addtag('f');
				packet->addf(slot->uf);
			}
			break;
	}
}

int makeSynthMsg(scpacket *packet, PyrSlot *slots, int size);
int makeSynthMsg(scpacket *packet, PyrSlot *slots, int size)
{
	packet->BeginMsg();
	
	for (int i=0; i<size; ++i) {
		addMsgSlot(packet, slots+i);
	}
	
	packet->EndMsg();
	return errNone;
}

int makeSynthMsgWithTags(scpacket *packet, PyrSlot *slots, int size);
int makeSynthMsgWithTags(scpacket *packet, PyrSlot *slots, int size)
{
	packet->BeginMsg();
	
	addMsgSlot(packet, slots); // msg address
	
	// skip space for tags
	packet->maketags(size);
	
	packet->addtag(',');

	for (int i=1; i<size; ++i) {
		addMsgSlotWithTags(packet, slots+i);
	}
		
	packet->EndMsg();

	return errNone;
}

void PerformOSCBundle(int inSize, char *inData, PyrObject *inReply);
void PerformOSCMessage(int inSize, char *inData, PyrObject *inReply);
PyrObject* ConvertReplyAddress(ReplyAddress *inReply);

void localServerReplyFunc(struct ReplyAddress *inReplyAddr, char* inBuf, int inSize);
void localServerReplyFunc(struct ReplyAddress *inReplyAddr, char* inBuf, int inSize)
{
    bool isBundle = IsBundle(inBuf);
    
    pthread_mutex_lock (&gLangMutex);
	if (compiledOK) {
		PyrObject *replyObj = ConvertReplyAddress(inReplyAddr);
		if (isBundle) {
			PerformOSCBundle(inSize, inBuf, replyObj);
		} else {
			PerformOSCMessage(inSize, inBuf, replyObj);
		}
	}
    pthread_mutex_unlock (&gLangMutex);
	
}

int makeSynthBundle(scpacket *packet, PyrSlot *slots, int size, bool useElapsed)
{
	double time;
	int err;
	int64 oscTime;
	
	err = slotDoubleVal(slots, &time);
	if (!err) {
		if (useElapsed) {
			oscTime = ElapsedTimeToOSC(time);
		} else {
			oscTime = (int64)(time * kSecondsToOSC);
		}
	} else {
		oscTime = 1;	// immediate
	}
	packet->OpenBundle(oscTime);
	
	for (int i=1; i<size; ++i) {
		if (isKindOfSlot(slots+i, class_array)) {
			PyrObject *obj = slots[i].uo;
			makeSynthMsgWithTags(packet, obj->slots, obj->size);
		}
	}
	packet->CloseBundle();
	return errNone;
}

int netAddrSend(PyrObject *netAddrObj, int msglen, char *bufptr, bool sendMsgLen=true);
int netAddrSend(PyrObject *netAddrObj, int msglen, char *bufptr, bool sendMsgLen)
{
	int err, port, addr;

	SC_TcpClientPort* comPort = (SC_TcpClientPort*)(netAddrObj->slots + ivxNetAddr_Socket)->uptr;

	if (comPort) {
		// send TCP
		int tcpSocket = comPort->Socket();

		if (sendMsgLen) {
			// send length of message in network byte-order
			int32 sizebuf = htonl(msglen);
			sendall(tcpSocket, &sizebuf, sizeof(int32));
		}

		sendall(tcpSocket, bufptr, msglen);

	} else {
		if (gUDPport == 0) return errFailed;

		// send UDP
		err = slotIntVal(netAddrObj->slots + ivxNetAddr_Hostaddr, &addr);
		if (err) return err;
		
		if (addr == 0) {
#ifdef SC_WIN32
      // no internal server under SC_WIN32 yet
#else
			if (gInternalSynthServer.mWorld) {
				World_SendPacket(gInternalSynthServer.mWorld, msglen, bufptr, &localServerReplyFunc);
			}
#endif
			return errNone;
		}

		err = slotIntVal(netAddrObj->slots + ivxNetAddr_PortID, &port);
		if (err) return err;		
		
		struct sockaddr_in toaddr;
		makeSockAddr(toaddr, addr, port);
	
		sendallto(gUDPport->Socket(), bufptr, msglen, (sockaddr*)&toaddr, sizeof(toaddr));
	}

	return errNone;
}


///////////

inline int OSCStrLen(char *str) 
{
	return (strlen(str) + 4) & ~3;
}


int makeSynthBundle(scpacket *packet, PyrSlot *slots, int size, bool useElapsed);

static void netAddrTcpClientNotifyFunc(void *clientData);
void netAddrTcpClientNotifyFunc(void *clientData)
{
	extern bool compiledOK;

	pthread_mutex_lock(&gLangMutex);
	if (compiledOK) {
		PyrObject* netAddrObj = (PyrObject*)clientData;
		VMGlobals* g = gMainVMGlobals;
		g->canCallOS = false;
		++g->sp; SetObject(g->sp, netAddrObj);
		runInterpreter(g, getsym("prConnectionClosed"), 1);
		g->canCallOS = false;
	}
	pthread_mutex_unlock(&gLangMutex);
}

int prNetAddr_Connect(VMGlobals *g, int numArgsPushed);
int prNetAddr_Connect(VMGlobals *g, int numArgsPushed)
{	
	PyrSlot* netAddrSlot = g->sp;
	PyrObject* netAddrObj = netAddrSlot->uo;
	
	int err, port, addr;
	
	err = slotIntVal(netAddrObj->slots + ivxNetAddr_PortID, &port);
	if (err) return err;
	
	err = slotIntVal(netAddrObj->slots + ivxNetAddr_Hostaddr, &addr);
	if (err) return err;
	
	struct sockaddr_in toaddr;
	makeSockAddr(toaddr, addr, port);
	
    int aSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (aSocket == -1) {
        post("\nCould not create socket\n");
		return errFailed;
	}
	
	const int on = 1;
#ifdef SC_WIN32
	if (setsockopt( aSocket, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on)) != 0) {
#else
  if (setsockopt( aSocket, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) != 0) {
#endif
    post("\nCould not setsockopt TCP_NODELAY\n");
#ifdef SC_WIN32
		closesocket(aSocket);
#else
		close(aSocket);
#endif
		return errFailed;
	};
	

    if(connect(aSocket,(struct sockaddr*)&toaddr,sizeof(toaddr)) != 0)
    {
        post("\nCould not connect socket\n");
#ifdef SC_WIN32
		    closesocket(aSocket);
#else
    		close(aSocket);
#endif
        return errFailed;
    }
	
	SC_TcpClientPort *comPort = new SC_TcpClientPort(aSocket, netAddrTcpClientNotifyFunc, netAddrObj);
	SetPtr(netAddrObj->slots + ivxNetAddr_Socket, comPort);

	return errNone;
}

int prNetAddr_Disconnect(VMGlobals *g, int numArgsPushed);
int prNetAddr_Disconnect(VMGlobals *g, int numArgsPushed)
{	
	PyrSlot* netAddrSlot = g->sp;
	PyrObject* netAddrObj = netAddrSlot->uo;
	
	SC_TcpClientPort *comPort = (SC_TcpClientPort*)(netAddrObj->slots + ivxNetAddr_Socket)->uptr;
	if (comPort) comPort->Close();

	return errNone;
}


int prNetAddr_SendMsg(VMGlobals *g, int numArgsPushed);
int prNetAddr_SendMsg(VMGlobals *g, int numArgsPushed)
{	
	PyrSlot* netAddrSlot = g->sp - numArgsPushed + 1;
	PyrSlot* args = netAddrSlot + 1;
	scpacket packet;
	
	int numargs = numArgsPushed - 1;
	makeSynthMsgWithTags(&packet, args, numargs);

	//for (int i=0; i<packet.size()/4; i++) post("%d %08X\n", i, packet.buf[i]);

	return netAddrSend(netAddrSlot->uo, packet.size(), (char*)packet.buf);
}


int prNetAddr_SendBundle(VMGlobals *g, int numArgsPushed);
int prNetAddr_SendBundle(VMGlobals *g, int numArgsPushed)
{	
	PyrSlot* netAddrSlot = g->sp - numArgsPushed + 1;
	PyrSlot* args = netAddrSlot + 1;
	scpacket packet;
	
	double time;
	int err = slotDoubleVal(args, &time);
	if (!err) {
		time += g->thread->seconds.uf;
		SetFloat(args, time);
	}
	int numargs = numArgsPushed - 1;
	makeSynthBundle(&packet, args, numargs, true);
	
	//for (int i=0; i<packet.size()/4; i++) post("%d %08X\n", i, packet.buf[i]);
	
	return netAddrSend(netAddrSlot->uo, packet.size(), (char*)packet.buf);
}

int prNetAddr_SendRaw(VMGlobals *g, int numArgsPushed);
int prNetAddr_SendRaw(VMGlobals *g, int numArgsPushed)
{	
	PyrSlot* netAddrSlot = g->sp - 1;
	PyrSlot* arraySlot = g->sp;
	PyrObject* netAddrObj = netAddrSlot->uo;
	
	if (!IsObj(arraySlot) || !isKindOf(arraySlot->uo, class_rawarray)) {
		error("sendRaw arg must be a kind of RawArray.\n");
		return errWrongType;
	}
	PyrObject *array = arraySlot->uo;
	
	char *bufptr = (char*)array->slots;
	int32 msglen = array->size * gFormatElemSize[array->obj_format];
	
	return netAddrSend(netAddrObj, msglen, bufptr, false);
}

int prNetAddr_BundleSize(VMGlobals *g, int numArgsPushed);
int prNetAddr_BundleSize(VMGlobals *g, int numArgsPushed)
{	
	PyrSlot* args = g->sp;
	scpacket packet;
	int numargs = args->uo->size;
	if (numargs < 1) return errFailed;
	makeSynthBundle(&packet, args->uo->slots, numargs, true);
	SetInt(args, packet.size());
	return errNone;
}

int prNetAddr_MsgSize(VMGlobals *g, int numArgsPushed);
int prNetAddr_MsgSize(VMGlobals *g, int numArgsPushed)
{	
	PyrSlot* args = g->sp;
	scpacket packet;
	
	int numargs = args->uo->size;
	if (numargs < 1) return errFailed;
	makeSynthMsgWithTags(&packet, args, numargs);
	SetInt(args, packet.size());
	return errNone;
}


int prNetAddr_UseDoubles(VMGlobals *g, int numArgsPushed);
int prNetAddr_UseDoubles(VMGlobals *g, int numArgsPushed)
{	
	//PyrSlot* netAddrSlot = g->sp - 1;
	PyrSlot* flag = g->sp;

	gUseDoubles = IsTrue(flag);
	
	return errNone;
}

int prArray_OSCBytes(VMGlobals *g, int numArgsPushed);
int prArray_OSCBytes(VMGlobals *g, int numArgsPushed)
{	
	PyrSlot* a = g->sp;
	PyrObject *array = a->uo;
	PyrSlot* args = array->slots;
	int numargs = array->size;
	if (numargs < 1) return errFailed;
	scpacket packet;
	
	if (IsFloat(args) || IsNil(args) || IsInt(args)) {
		makeSynthBundle(&packet, args, numargs, false);
	} else if (IsSym(args) || isKindOfSlot(args, class_string)) {
		makeSynthMsgWithTags(&packet, args, numargs);
	} else {
		return errWrongType;
	}
	
	int size = packet.size();
	PyrInt8Array* obj = newPyrInt8Array(g->gc, size, 0, true);
	obj->size = size;
	memcpy(obj->b, packet.data(), size);
	SetObject(a, (PyrObject*)obj);
	//for (int i=0; i<packet.size()/4; i++) post("%d %08X\n", i, packet.buf[i]);
	
	return errNone;
}

// Create a new <PyrInt8Array> object and copy data from `msg.getb'.
// Bytes are properly untyped, but there is no <UInt8Array> type.
 
static PyrInt8Array* MsgToInt8Array ( sc_msg_iter msg ) ;
static PyrInt8Array* MsgToInt8Array ( sc_msg_iter msg )
{
	int size = msg.getbsize() ;
	VMGlobals *g = gMainVMGlobals ;
	PyrInt8Array *obj = newPyrInt8Array ( g->gc , size , 0 , true ) ;
	obj->size = size ;
	msg.getb ( (char *)obj->b , obj->size ) ;
	return obj ;
}

PyrObject* ConvertOSCMessage(int inSize, char *inData)
{
	char *cmdName = inData;
	int cmdNameLen = OSCstrlen(cmdName);
	sc_msg_iter msg(inSize - cmdNameLen, inData + cmdNameLen);
        
	int numElems;
        if (inSize == cmdNameLen) {
            numElems = 0;
        } else {
			if (!msg.tags) {
				numElems = 0;
				error("OSC messages must have type tags.  %s\n", cmdName);
			} else {
				numElems = strlen(msg.tags);
			}
        }
        //post("tags %s %d\n", msg.tags, numElems);
        
        VMGlobals *g = gMainVMGlobals;
        PyrObject *obj = newPyrArray(g->gc, numElems + 1, 0, false);
        PyrSlot *slots = obj->slots;

        SetSymbol(slots+0, getsym(cmdName));
        
        for (int i=0; i<numElems; ++i) {
            char tag = msg.nextTag();
            //post("%d %c\n", i, tag);
            switch (tag) {
                case 'i' :
                    SetInt(slots+i+1, msg.geti());
                    break;
                case 'f' :
                    SetFloat(slots+i+1, msg.getf());
                    break;
                case 'd' :
                    SetFloat(slots+i+1, msg.getd());
                    break;
                case 's' :
                    SetSymbol(slots+i+1, getsym(msg.gets()));
                    //post("sym '%s'\n", slots[i+1].us->name);
                    break;
                case 'b' :
		    SetObject(slots+i+1, (PyrObject*)MsgToInt8Array(msg));
                    break;
            }
        }
        obj->size = numElems + 1;
        return obj;
}

PyrObject* ConvertReplyAddress(ReplyAddress *inReply)
{
    VMGlobals *g = gMainVMGlobals;
    PyrObject *obj = instantiateObject(g->gc, s_netaddr->u.classobj, 2, true, false);
    PyrSlot *slots = obj->slots;
    SetInt(slots+0, ntohl(inReply->mSockAddr.sin_addr.s_addr));
    SetInt(slots+1, ntohs(inReply->mSockAddr.sin_port));
    return obj;
}

void PerformOSCBundle(int inSize, char* inData, PyrObject *replyObj)
{
    // convert all data to arrays
    
    int64 oscTime = OSCtime(inData + 8);
    double seconds = OSCToElapsedTime(oscTime);

    VMGlobals *g = gMainVMGlobals;
    ++g->sp; SetObject(g->sp, g->process);
    ++g->sp; SetFloat(g->sp, seconds);
    ++g->sp; SetObject(g->sp, replyObj);
    
    PyrSlot *stackBase = g->sp;
    char *data = inData + 16;
    char* dataEnd = inData + inSize;
    while (data < dataEnd) {
        int32 msgSize = OSCint(data);
        data += sizeof(int32);
        PyrObject *arrayObj = ConvertOSCMessage(msgSize, data);
        ++g->sp; SetObject(g->sp, arrayObj);
        data += msgSize;
    }
	
	int numMsgs = g->sp - stackBase;
	
    runInterpreter(g, s_recvoscbndl, 3+numMsgs);
}

void ConvertOSCBundle(int inSize, char* inData, PyrObject *replyObj)
{
    // convert all data to arrays
    
    //int64 oscTime = OSCtime(inData + 8);
    //double seconds = OSCToElapsedTime(oscTime);

    VMGlobals *g = gMainVMGlobals;
    
    int numMsgs = 0;
    char *data = inData + 16;
    char* dataEnd = inData + inSize;
    while (data < dataEnd) {
        int32 msgSize = OSCint(data);
        data += sizeof(int32);
        PyrObject *arrayObj = ConvertOSCMessage(msgSize, data);
        ++g->sp; SetObject(g->sp, arrayObj);
        numMsgs++;
        data += msgSize;
    }
}

void PerformOSCMessage(int inSize, char *inData, PyrObject *replyObj)
{
    
    PyrObject *arrayObj = ConvertOSCMessage(inSize, inData);
   
    // call virtual machine to handle message
    VMGlobals *g = gMainVMGlobals;
    ++g->sp; SetObject(g->sp, g->process);
    ++g->sp; SetFloat(g->sp, elapsedTime());	// time
    ++g->sp; SetObject(g->sp, replyObj);
    ++g->sp; SetObject(g->sp, arrayObj);	
	
    runInterpreter(g, s_recvoscmsg, 4);
	

}

void FreeOSCPacket(OSC_Packet *inPacket)
{
    //post("->FreeOSCPacket %08X\n", inPacket);
    if (inPacket) {
            free(inPacket->mData);
            free(inPacket);
    }
}

void ProcessOSCPacket(OSC_Packet* inPacket)
{
    //post("recv '%s' %d\n", inPacket->mData, inPacket->mSize);
	inPacket->mIsBundle = IsBundle(inPacket->mData);
    
    pthread_mutex_lock (&gLangMutex);
	if (compiledOK) {
		PyrObject *replyObj = ConvertReplyAddress(&inPacket->mReplyAddr);
		if (compiledOK) {
			if (inPacket->mIsBundle) {
				PerformOSCBundle(inPacket->mSize, inPacket->mData, replyObj);
			} else {
				PerformOSCMessage(inPacket->mSize, inPacket->mData, replyObj);
			}
		}
	}
    pthread_mutex_unlock (&gLangMutex);

    FreeOSCPacket(inPacket);
}

void init_OSC(int port);
void init_OSC(int port)
{	
    postfl("init_OSC\n");
    try {
        gUDPport = new SC_UdpInPort(port);
    } catch (...) {
        postfl("No networking.");
    }
}

int prGetHostByName(VMGlobals *g, int numArgsPushed);
int prGetHostByName(VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a = g->sp;
	char hostname[256];
	
	int err = slotStrVal(a, hostname, 255);
	if (err) return err;
		
	struct hostent *he = gethostbyname(hostname);
	if (!he) return errFailed;
	
	SetInt(a, ntohl(*(int*)he->h_addr));
	
	return errNone;
}

int prExit(VMGlobals *g, int numArgsPushed);
int prExit(VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a = g->sp;
	
	exit(a->ui);
        
	//post("exit %d\n", a->ui);
	//DumpBackTrace(g);
	return errNone;
}

extern "C" {
	int vpost(const char *fmt, va_list vargs);
}

#ifndef SC_WIN32
int prBootInProcessServer(VMGlobals *g, int numArgsPushed);
int prBootInProcessServer(VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a = g->sp;
	
	if (!gInternalSynthServer.mWorld) {
		SetPrintFunc(&vpost);
		WorldOptions options = kDefaultWorldOptions;
		
		PyrObject *optionsObj = a->uo;
		PyrSlot *optionsSlots = optionsObj->slots;

		int err;
		
		err = slotIntVal(optionsSlots + 0, (int*)&options.mNumAudioBusChannels);
		if (err) return err;
		
		err = slotIntVal(optionsSlots + 1, (int*)&options.mNumControlBusChannels);
		if (err) return err;
		
		err = slotIntVal(optionsSlots + 2, (int*)&options.mNumInputBusChannels);
		if (err) return err;
		
		err = slotIntVal(optionsSlots + 3, (int*)&options.mNumOutputBusChannels);
		if (err) return err;
		
		err = slotIntVal(optionsSlots + 4, (int*)&options.mNumBuffers);
		if (err) return err;
		
		err = slotIntVal(optionsSlots + 5, (int*)&options.mMaxNodes);
		if (err) return err;
		
		err = slotIntVal(optionsSlots + 6, (int*)&options.mMaxGraphDefs);
		if (err) return err;
				
		err = slotIntVal(optionsSlots + 8, (int*)&options.mBufLength);
		if (err) return err;
		
		if (NotNil(optionsSlots + 9)) {
			err = slotIntVal(optionsSlots + 9, (int*)&options.mPreferredHardwareBufferFrameSize);
			if (err) return err;
		}
		
		err = slotIntVal(optionsSlots + 10, (int*)&options.mRealTimeMemorySize);
		if (err) return err;
		
		err = slotIntVal(optionsSlots + 11, (int*)&options.mNumRGens);
		if (err) return err;
		
		err = slotIntVal(optionsSlots + 12, (int*)&options.mMaxWireBufs);
		if (err) return err;
		
		if (NotNil(optionsSlots + 13)) {
			err = slotIntVal(optionsSlots + 13, (int*)&options.mPreferredSampleRate);
			if (err) return err;
		}
		
		options.mLoadGraphDefs = IsTrue(optionsSlots + 14) ? 1 : 0;
		
		options.mNumSharedControls = gInternalSynthServer.mNumSharedControls;
		options.mSharedControls = gInternalSynthServer.mSharedControls;
		
		gInternalSynthServer.mWorld = World_New(&options);
	}
	
	return errNone;
}

int getScopeBuf(uint32 index, SndBuf *buf, bool& didChange)
{	
	if (gInternalSynthServer.mWorld) {
		int serverErr = World_CopySndBuf(gInternalSynthServer.mWorld, index, buf, true, didChange);
		if (serverErr) return errFailed;
	} else {
		didChange = false;
	}
	return errNone;
}
void* wait_for_quit(void* thing);
void* wait_for_quit(void* thing)
{
	World *world = (World*)thing;
	World_WaitForQuit(world);
	return 0;
}

int prQuitInProcessServer(VMGlobals *g, int numArgsPushed);
int prQuitInProcessServer(VMGlobals *g, int numArgsPushed)
{
	//PyrSlot *a = g->sp;
	
	if (gInternalSynthServer.mWorld) {
		World *world = gInternalSynthServer.mWorld;
		gInternalSynthServer.mWorld = 0;
		
        pthread_t thread;
        pthread_create(&thread, NULL, wait_for_quit, (void*)world);
		pthread_detach(thread);
	}
	
	return errNone;
}
#endif
//#ifndef SC_WIN32

inline int32 BUFMASK(int32 x)
{
	return (1 << (31 - CLZ(x))) - 1;
}

int prAllocSharedControls(VMGlobals *g, int numArgsPushed);
int prAllocSharedControls(VMGlobals *g, int numArgsPushed)
{
	//PyrSlot *a = g->sp - 1;
	PyrSlot *b = g->sp;
	
	if (gInternalSynthServer.mWorld) {
		post("can't allocate while internal server is running\n");
		return errNone;
	}
	if (gInternalSynthServer.mSharedControls != gDefaultSharedControls) {
		free(gInternalSynthServer.mSharedControls);
		gInternalSynthServer.mSharedControls = gDefaultSharedControls;
	}
	int numSharedControls;
	int err = slotIntVal(b, &numSharedControls);
	if (err) return err;
	if (numSharedControls <= 0) {
		gInternalSynthServer.mNumSharedControls = 0;
	} else if (numSharedControls < kNumDefaultSharedControls) {
		gInternalSynthServer.mNumSharedControls = numSharedControls;
	} else {
		gInternalSynthServer.mNumSharedControls = numSharedControls;
		gInternalSynthServer.mSharedControls = (float*)calloc(numSharedControls, sizeof(float));
	}
	return errNone;
}


int prGetSharedControl(VMGlobals *g, int numArgsPushed);
int prGetSharedControl(VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a = g->sp - 1;
	PyrSlot *b = g->sp;
	
	int index;
	int err = slotIntVal(b, &index);
	if (err) return err;
	if (index < 0 || index >= gInternalSynthServer.mNumSharedControls) {
		SetFloat(a, 0.);
		return errNone;
	}
	float val = gInternalSynthServer.mSharedControls[index];
	SetFloat(a, val);
	return errNone;
}

int prSetSharedControl(VMGlobals *g, int numArgsPushed);
int prSetSharedControl(VMGlobals *g, int numArgsPushed)
{
	//PyrSlot *a = g->sp - 2;
	PyrSlot *b = g->sp - 1;
	PyrSlot *c = g->sp;
	
	int index;
	int err = slotIntVal(b, &index);
	if (err) return err;
	
	float val;
	err = slotFloatVal(c, &val);
	if (err) return err;
	
	if (index < 0 || index >= gInternalSynthServer.mNumSharedControls) {
		return errNone;
	}
	gInternalSynthServer.mSharedControls[index] = val;
	return errNone;
}

void init_OSC_primitives();
void init_OSC_primitives()
{
	int base, index;
	
	base = nextPrimitiveIndex();
	index = 0;

	definePrimitive(base, index++, "_NetAddr_Connect", prNetAddr_Connect, 1, 0);	
	definePrimitive(base, index++, "_NetAddr_Disconnect", prNetAddr_Disconnect, 1, 0);	
	definePrimitive(base, index++, "_NetAddr_SendMsg", prNetAddr_SendMsg, 1, 1);	
	definePrimitive(base, index++, "_NetAddr_SendBundle", prNetAddr_SendBundle, 2, 1);	
	definePrimitive(base, index++, "_NetAddr_SendRaw", prNetAddr_SendRaw, 2, 0);	
	definePrimitive(base, index++, "_NetAddr_BundleSize", prNetAddr_BundleSize, 1, 0);	
	definePrimitive(base, index++, "_NetAddr_MsgSize", prNetAddr_MsgSize, 1, 0);	

	definePrimitive(base, index++, "_NetAddr_UseDoubles", prNetAddr_UseDoubles, 2, 0);	
	definePrimitive(base, index++, "_Array_OSCBytes", prArray_OSCBytes, 1, 0);	
	definePrimitive(base, index++, "_GetHostByName", prGetHostByName, 1, 0);	
	definePrimitive(base, index++, "_Exit", prExit, 1, 0);	
#ifndef SC_WIN32
  definePrimitive(base, index++, "_BootInProcessServer", prBootInProcessServer, 1, 0);	
	definePrimitive(base, index++, "_QuitInProcessServer", prQuitInProcessServer, 1, 0);
#endif
	definePrimitive(base, index++, "_AllocSharedControls", prAllocSharedControls, 2, 0);	
	definePrimitive(base, index++, "_SetSharedControl", prSetSharedControl, 3, 0);	
	definePrimitive(base, index++, "_GetSharedControl", prGetSharedControl, 2, 0);	

	//post("initOSCRecs###############\n");
	s_call = getsym("call");
	s_write = getsym("write");
	s_recvoscmsg = getsym("recvOSCmessage");
	s_recvoscbndl = getsym("recvOSCbundle");
	s_netaddr = getsym("NetAddr");
}



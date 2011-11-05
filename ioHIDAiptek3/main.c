/* 
 (c) Udo Killermann 2011
 
 Tablet State and Event Processing taken in major parts from
 Tablet Magic Daemon Sources (c) 2011 Thinkyhead Software
 
 Aiptek Report Decoding and Command Codes taken from Linux 2.6
 Kernel Driver aiptek.c
 --
 Copyright (c) 2001      Chris Atenasio   <chris@crud.net>
 Copyright (c) 2002-2004 Bryan W. Headley <bwheadley@earthlink.net>
 
 based on wacom.c by
 Vojtech Pavlik      <vojtech@suse.cz>
 Andreas Bach Aaen   <abach@stofanet.dk>
 Clifford Wolf       <clifford@clifford.at>
 Sam Mosel           <sam.mosel@computer.org>
 James E. Blair      <corvus@gnu.org>
 Daniel Egger        <egger@suse.de>
 --
 
 LICENSE
 
 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU Library General Public
 License as published by the Free Software Foundation; either
 version 3 of the License, or (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Library General Public License for more details.
 
 You should have received a copy of the GNU Library General Public
 License along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <paths.h>
#include <termios.h>
#include <sysexits.h>
#include <sys/param.h>
#include <sys/select.h>
#include <sys/time.h>

#include <CarbonCore/CarbonCore.h>


#include <CoreFoundation/CoreFoundation.h>
#include <Carbon/Carbon.h>


#include <IOKit/IOKitLib.h>

#include <IOKit/hid/IOHIDLib.h>

#include <IOKit/serial/IOSerialKeys.h>
#include <IOKit/IOBSD.h>

#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/hidsystem/IOHIDShared.h>

#include "stylus.h"		// structures and constants needed for TabletMagic's functions to work
#include "aiptek.h"		// background information needed to understand tablet's reports and
						// functions - taken from Linux' kernel driver
// #include "vkeys.h"

// Aiptek
#define VendorID  0x08ca

// HyperPen 12000U
#define ProductID 0x0010


void PostNXEvent(int eventType, SInt16 eventSubType, UInt8 otherButton);
void PostChangeEvents();

IOHIDDeviceRef currentDeviceRef=NULL;

// the following sequences are sent to the tablet to set it into absolute tablet mode with
// macrokey support - setting these commands is done with IOHIDSetReport
// unfortunately IOHIDGetReport doesn't work as expected so at the moment I refrain to query 
// the devices capabilities
//
uint8_t switchToTablet[]={0x02,0x10,0x01};
uint8_t enableMacroKeys[]={0x02,0x11,0x02};
uint8_t	autoGainOn[]={0x02,0x12,0xff};
uint8_t filterOn[]={0x02,0x17,0x00};
uint8_t setResolution[]={0x02,0x18,0x04};
uint8_t switchToMouse[]={0x02,0x10,0x00};

// query codes - will be used as soon as I get the interrupt report callback to work for me
// I need to step back to the old HID API in order to do so
uint8_t getXExtension[]={0x02,0x01,0x00};
uint8_t getYExtension[]={0x02,0x01,0x01};
uint8_t getPressureLevels[]={0x02,0x08,0x00};

// some information is sent as bit field stored in an 8 bit value
// to interpret them I define the following masks
const uint8_t DVmask=0x01;
const uint8_t IRmask=0x02;
const uint8_t TIPmask=0x04;
const uint8_t BS1mask=0x08;
const uint8_t BS2mask=0x10;

// button mapping is different if we are running the tablet in relative
// mode (aka mouse mode)
const uint8_t relTIPmask=0x01;
const uint8_t relBS1mask=0x02;
const uint8_t relBS2mask=0x04;

uint8_t buffer[32];
mach_port_t		io_master_port;		//!< The master port for HID events
io_connect_t	gEventDriver;		//!< The connection by which HID events are sent

// globall state to handle processing of changes of system state (screen and tablet extensions)
CGRect	screenBounds;
CGRect   tabletBounds;
CGRect screenMapping;
CGRect tabletMapping;

// global state to handle processing of state change from event to event
StylusState stylus;
StylusState oldStylus;

// button handling is done within a 16 Bit integer used as bit field
bool			buttonState[kSystemClickTypes];		//!< The state of all the system-level buttons
bool			oldButtonState[kSystemClickTypes];	//!< The previous state of all system-level buttons

int		button_mapping[] = { kSystemButton1, kSystemButton1, kSystemButton2, kSystemEraser };

#define SetButtons(x)		{stylus.button_click=((x)!=0);stylus.button_mask=x;int qq;for(qq=kButtonMax;qq--;stylus.button[qq]=((x)&(1<<qq))!=0);}
#define ResetButtons		SetButtons(0)


// satisfying some needs of tabletMagic's code
bool mouse_mode;
int mouse_scaling=1;
bool tablet_on=TRUE;

// naming the buttons to do some debugging
char * buttonNames[] = {"dummy","button 1","button 2","button 3","button 4","button 5",
						"button 6","button 7","button 8", "button 9","button 10","button 11",
						"button 12","button 13","button 14", "button 15","button 16","button 17",
						"button 18","button 19","button 20", "button 21","button 22","button 23","button 24","dummy"};




// forward declare
static void theInputReportCallback(void *context, IOReturn inResult, void * inSender, IOHIDReportType inReportType, 
								   uint32_t reportID, uint8_t *inReport, CFIndex length);
void InitTabletBounds(SInt32 x1, SInt32 y1, SInt32 x2, SInt32 y2);

void HIDPostVirtualModifier(UInt32 gModifiers);
void HIDPostVirtualKey(const UInt8 inVirtualKeyCode, const Boolean inPostUp, const Boolean inRepeat);

// unschedule events fired by the device from run loop
void theDeviceRemovalCallback (void *context, IOReturn result, void *sender, IOHIDDeviceRef device)
{
	currentDeviceRef=NULL;
	IOHIDDeviceUnscheduleFromRunLoop( device, CFRunLoopGetCurrent( ), kCFRunLoopDefaultMode );
	fprintf(stderr,"Tablet removed!\n");
}

// issueCommand()
// instructs the tablet to execute a specific command
// it's a convenience function, so I don't have to write down the SetReport everytime
// I need to access it
IOReturn issueCommand(IOHIDDeviceRef deviceRef, uint8_t * command)
{
	return IOHIDDeviceSetReport( deviceRef,
								kIOHIDReportTypeFeature,
								2,
								command,
								3);
}

// theDeviceMatchingTablet() is called as soon as either a device is already plugged in when the
// application is starting or the device is connected while the application is running
//
// initialize the matching device (aiptek tablet) and register the event handler with the run loop
// afterwards input reports will be send to this event handler 
void theDeviceMatchingCallback(void *inContext, IOReturn inResult, void *inSender, IOHIDDeviceRef  inIOHIDDeviceRef)
{
	IOReturn ioReturn;
	
	currentDeviceRef=inIOHIDDeviceRef;
	
	// initialize the tablet to get it going
	
	// turn on digitizer mode
	ioReturn=issueCommand(inIOHIDDeviceRef, switchToTablet);
	mouse_mode=FALSE;
	
	// turn on macro key mode
	ioReturn=issueCommand(inIOHIDDeviceRef, enableMacroKeys);
	
	// turn on auto gain
	ioReturn=issueCommand(inIOHIDDeviceRef,autoGainOn);
	
	
	// turn on filter
	// ioReturn=issueCommand(currentDeviceRef, filterOn);
	
	
	// set Resolution
	// ioReturn=issueCommand(currentDeviceRef, setResolution);	
	
	
	fprintf(stderr,"Tablet connected!\n");
	IOHIDDeviceScheduleWithRunLoop(inIOHIDDeviceRef, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
	IOHIDDeviceRegisterInputReportCallback(inIOHIDDeviceRef, buffer, 512, theInputReportCallback, "hund katze maus");
	
	
}


// handleAbsoluteReport() is the entry point for processing the
// digitizer information if running in absolute mode
//
// it takes the HID devices' raw report and interprets it according
// to the report definition
// 
// starting byte can be ignored this has alreday been processed by
// the HID report callback (otherwise we wouldn't have ended here)
//
// essantially this routine converts the raw information received by the Aiptek Tablet
// to the logical tablet and stylus state represented by TabletMagic's code
//
void handleAbsoluteReport(uint8_t * inReport)
{
	int xCoord;
	int yCoord;
	int tipPressure;
	UInt16	bm = 0; // button mask
	
	ResetButtons;  // forget the system buttons and reconstruct them in this routine
	
	xCoord=inReport[1]|inReport[2]<<8;
	yCoord=inReport[3]|inReport[4]<<8;
	
	tipPressure=inReport[6]|inReport[7]<<8;

#ifdef DEBUG
	printf("xCoord: %04d\t",xCoord);
	printf("yCoord: %04d\t",yCoord);
	printf("Tip Pressure: %03d\t\t",tipPressure);
	if (inReport[5] & DVmask) printf("DV "); else printf("-- ");
	if (inReport[5] & IRmask) printf("IR "); else printf("-- "); // generate Proximity Event
	if (inReport[5] & TIPmask) printf("TIP "); else printf("--- ");
	if (inReport[5] & BS1mask) printf("BS1 "); else printf("--- ");
	if (inReport[5] & BS2mask) printf("BS2 "); else printf("--- ");
#endif	
		
	// Remember the old position for tracking relative motion
	stylus.old.x = stylus.point.x;
	stylus.old.y = stylus.point.y;
	
	// store new postion
	stylus.point.x=xCoord;
	stylus.point.y=yCoord;
	
	// calculate difference
	stylus.motion.x = stylus.point.x - stylus.old.x;
	stylus.motion.y = stylus.point.y - stylus.old.y; // point und old werden in tablet koordinaten übertragen
	
	
	// constrain tip pressure to 511 (9 Bit)	
	
	tipPressure=(tipPressure>511)?511:tipPressure;
	
	// tablet events are scaled to 0xFFFF (16 bit), so
	// a little shift to the right is needed
	stylus.pressure=tipPressure<<7; 
	
	// reconstruct the button state
	// button state is or'ed together using the corresponding bit masks
	
	if (inReport[5] & TIPmask) {
		bm|=kBitStylusTip;
	}
	
	if (inReport[5] & BS1mask) {
		bm|=kBitStylusButton1;
	}
	
	if (inReport[5] & BS2mask) {
		bm|=kBitStylusButton2;
	}
	
	// set the button state in the current stylus state
	SetButtons(bm);
	
	//
	// proximity
	//
	
	stylus.off_tablet = ((inReport[5] & IRmask)==0)?TRUE:FALSE;	
	
}


// 
// Relative reports (mouse mode) deliver values in 2's complement format to
// deal with negative offsets.
static int aiptek_convert_from_2s_complement(uint8_t c)
{
	int ret;
	uint8_t b = c;
	int negate = 0;
	
	if ((b & 0x80) != 0) {
		b--;
		b = ~b;
		
		negate = 1;
	}
	ret = b;
	ret = (negate == 1) ? -ret : ret;
	return ret;
}

// event pump enters here
// callback is registered with the Aiptek tablet device
// packet decoding is based on information provided in Linux's
// Aiptek tablet driver
//

static void theInputReportCallback(void *context, IOReturn inResult, void * inSender, IOHIDReportType inReportType, 
								   uint32_t reportID, uint8_t *inReport, CFIndex length)
{	
	int deltaX;
	int deltaY;
	UInt16	bm = 0; // button mask
	
	static uint8_t key; // macro key handling
		
	switch (reportID) {
		// this needs to be refactored, so I don't interpret the commands directly in the switch
		// but delegate similar to the absolute reports
		//
		case kRelativePointer:
			ResetButtons;
			deltaX=aiptek_convert_from_2s_complement(inReport[2]);
			
			printf("delta X: %d\n",deltaX);
			
			deltaY=aiptek_convert_from_2s_complement(inReport[3]);

			
	
			printf("delta Y: %d\n",deltaY);
			
			stylus.old.x=stylus.point.x;
			stylus.old.y=stylus.point.y;
			
			stylus.point.x=stylus.old.x+deltaX;
			stylus.point.y=stylus.old.y+deltaY;
			
			stylus.motion.x=deltaX;
			stylus.motion.y=deltaY;
			
			printf("\t\tbutton state: %x",inReport[1]);
			
			
			if (inReport[1] & relTIPmask) {
				bm|=kBitStylusTip;
			}
			
			// my mouse doesn't support the middle button in relative mode, so I've to 
			// hack the right mouse to behave like it should - I map both none left
			// mouse buttons to StylusButton2
			if (inReport[1] & relBS1mask) {
				bm|=kBitStylusButton2;
			}

			
			if (inReport[1] & relBS2mask) {
				bm|=kBitStylusButton2;
			}
			
			// set the button state in the current stylus state
			SetButtons(bm);
			
			PostChangeEvents();
			
			break;
			
		case kAbsoluteStylus:
			handleAbsoluteReport(inReport);
#ifdef DEBUG
			puts("\tStylus\n");
#endif
			PostChangeEvents();
			
			break;
			
		case kAbsoluteMouse:
			
			handleAbsoluteReport(inReport);
#ifdef DEBUG
			puts("\tMouse\n");
#endif
			PostChangeEvents();
			
			break;
			
		case kMacroStylus:
			if (inReport[1] & TIPmask) {
				key=inReport[3]/2;
			}
			else {
				if (inReport[3]/2==key) {
					printf("%s\n",buttonNames[key]);
					key=99;	// reset state
					// use soft keys to do some changes to the tablet's state
					switch (inReport[3]/2) {
						case 17:
							// HIDPostAuxKey(NX_SUBTYPE_STICKYKEYS_ON);
							// HIDPostAuxKey(NX_SUBTYPE_STICKYKEYS_SHIFT);
							// HIDPostVirtualModifiers(kVK_F10,FALSE,FALSE);
							HIDPostVirtualModifier(NX_ALTERNATEMASK|NX_SHIFTMASK);
							HIDPostVirtualKey(kVK_F6, FALSE, FALSE);
							HIDPostVirtualKey(kVK_F6, TRUE, FALSE);
							HIDPostVirtualModifier(0);
							break;
							
							
							
						case 18:
							HIDPostVirtualKey(kVK_F10, FALSE, FALSE);
							HIDPostVirtualKey(kVK_F10, TRUE, FALSE);
							break;
							
						
						case 19:
							HIDPostVirtualKey(kVK_F8, FALSE, FALSE);
							HIDPostVirtualKey(kVK_F8, TRUE, FALSE);
							break;
							
						case 20:
							HIDPostVirtualKey(0, FALSE, FALSE);
							HIDPostVirtualKey(0, TRUE, FALSE);
							break;
							
						case 21:
							InitTabletBounds(500, 500, 3500, 2750);
							puts("shrinking tablet bounds");
							break;
						case 22:
							InitTabletBounds(0, 0, 6000, 4500);
							puts("resetting tablet bounds");
							break;

						// acase 23:	
						case 30:
							issueCommand(currentDeviceRef, switchToMouse);
							mouse_mode=TRUE;
							stylus.point.x=3000;
							stylus.point.y=2250;
							stylus.off_tablet = FALSE;
							puts("switching to mouse (relative) mode");
							break;
						case 24:
							issueCommand(currentDeviceRef, switchToTablet);
							stylus.off_tablet = TRUE;
							mouse_mode=FALSE;
							break;
						default:
							break;
					}
				}
			}

			break;

		case kMacroMouse:
			if (inReport[1] & TIPmask) {
				key=inReport[3]/2;
			}
			else {
				if (inReport[3]/2==key) {
					printf("%s\n",buttonNames[key]);
					key=99;	// reset state
				}
			}
			
			break;
			
		default:
			break;
	}
	
}

//


void HIDPostVirtualModifier(UInt32 gModifiers)
{
	NXEventData eventData;
	IOGPoint  loc = { 0, 0 };
	
	bzero(&eventData, sizeof(NXEventData));
		
	IOHIDPostEvent(gEventDriver, NX_FLAGSCHANGED, loc, &eventData, kNXEventDataVersion, gModifiers , TRUE );
}
	
void HIDPostVirtualKey(
							  const UInt8 inVirtualKeyCode,
							  const Boolean inPostUp,
							  const Boolean inRepeat)
{
	NXEventData eventData;
	IOGPoint  loc = { 0, 0 };
	
	NXEvent event;
	
	bzero(&event, sizeof(NXEvent));
	bzero(&eventData, sizeof(NXEventData));
	
	// event.flags=NX_COMMANDMASK;
	// NX_FLAGSCHANGED
	
	eventData.key.repeat = inRepeat;
	eventData.key.keyCode = inVirtualKeyCode;
	eventData.key.origCharSet = eventData.key.charSet = NX_ASCIISET;
	eventData.key.origCharCode = eventData.key.charCode = 0x0;
	IOHIDPostEvent(gEventDriver, (inPostUp ? NX_KEYUP : NX_KEYDOWN), loc, &eventData, kNXEventDataVersion,  kIOHIDPostHIDManagerEvent , FALSE );
	
	// event.type=
}



// <Start of TabletMagic Code>

//
// ShortSleep
//
void ShortSleep2() {
	struct timespec rqtp = { 0, 50000000 };
	nanosleep(&rqtp, NULL);
}

// Short Sleep is not needed on my MacBook
// so I override it
#define ShortSleep() ;


//
// InitTabletBounds
//
// Initialize the active tablet region.
// This is called to set the initial mapping of the tablet.
// Command-line arguments might leave some of these set to -1.
// The rest are set when the tablet dimensions are received.
//
void InitTabletBounds(SInt32 x1, SInt32 y1, SInt32 x2, SInt32 y2) {
	tabletMapping.origin.x = (x1 != -1) ? x1 : 0;
	tabletMapping.origin.y = (y1 != -1) ? y1 : 0;
	tabletMapping.size.width = (x2 != -1) ? (x2 - tabletMapping.origin.x + 1) : 6000;
	tabletMapping.size.height = (y2 != -1) ? (y2 - tabletMapping.origin.y + 1) : 4500;
	
}



//
// UpdateDisplaysBounds
//
bool UpdateDisplaysBounds() {
	//	CGRect				activeDisplaysBounds;
	CGDirectDisplayID	*displays;
	CGDisplayCount		numDisplays;
	CGDisplayCount		i;
	CGDisplayErr		err;
	bool				result = false;
	
	screenBounds = CGRectMake(0.0, 0.0, 0.0, 0.0);
	
	err = CGGetActiveDisplayList(0, NULL, &numDisplays);
	
	if (err == CGDisplayNoErr && numDisplays > 0) {
		displays = (CGDirectDisplayID*)malloc(numDisplays * sizeof(CGDirectDisplayID));
		
		if (NULL != displays) {
			err = CGGetActiveDisplayList(numDisplays, displays, &numDisplays);
			
			if (err == CGDisplayNoErr)
				for (i = 0; i < numDisplays; i++)
					screenBounds = CGRectUnion(screenBounds, CGDisplayBounds(displays[i]));
			
			free(displays);
			result = true;
		}
	}
	
exit:
	printf("Screen Boundary: %.2f, %.2f - %.2f, %.2f\n", screenBounds.origin.x, screenBounds.origin.y, 
		   screenBounds.size.width, screenBounds.size.height);
	return result;
}

void SetScreenMapping(SInt16 x1, SInt16 y1, SInt16 x2, SInt16 y2) {
	x1 = (x1 == -1) ? 0 : x1;
	y1 = (y1 == -1) ? 0 : y1;
	
	screenMapping = CGRectMake(
							   x1, y1,
							   (x2 == -1) ? (screenBounds.size.width - x1) : x2 - x1 + 1,
							   (y2 == -1) ? (screenBounds.size.height - y1) : y2 - y1 + 1
							   );
}



//
// PostChangeEvents
//
//  Compare the current state to the previous state and send
//  all the events necessary to get the system up to speed.
//
//  This method is analogous to the HID queue provider. It
//  determines the changes in tablet state and directly calls
//  our NXEvent dispatcher, whereas the HID stuff relies on a
//  formal data provider which is usually at the kernel level.
//
void PostChangeEvents() {
	static bool	dragState = false;
	
	// printf("Screen Boundary: %.2f, %.2f - %.2f, %.2f\n", screenBounds.origin.x, screenBounds.origin.y, 
	//	   screenBounds.size.width, screenBounds.size.height);
	
	//
	// Get Screen and Tablet areas
	//
	/*
	CGFloat	swide = screenMapping.size.width, shigh = screenMapping.size.height,
	twide = tabletMapping.size.width, thigh = tabletMapping.size.height;
	*/
#define swide screenMapping.size.width
#define	shigh	screenMapping.size.height
#define	twide	tabletMapping.size.width
#define thigh	tabletMapping.size.height
	//
	// Get the Screen Boundary
	//
#define	sx1	screenMapping.origin.x
#define	sy1	screenMapping.origin.y
#define sx2 screenMapping.size.width
#define sy2 screenMapping.size.height

	/*
	CGFloat	sx1 = screenMapping.origin.x,
	sy1 = screenMapping.origin.y;
	
	CGFloat sx2=screenMapping.size.width;
	CGFloat sy2=screenMapping.size.height;
	*/
	//
	// And the Tablet Boundary
	//
	CGFloat	nx, ny,
	tx1 = tabletMapping.origin.x,
	tx2 = tx1 + twide - 1,
	ty1 = tabletMapping.origin.y,
	ty2 = ty1 + thigh - 1;
	
	if (mouse_mode) {
		
		//
		// Get the minimal ratio of the tablet to the screen
		// and use this to get reasonable starting mouse motion
		//
		CGFloat	hratio = screenBounds.size.width / twide,
		vratio = screenBounds.size.height / thigh,
		minratio = ((hratio < vratio) ? hratio : vratio) * 2.0f;
		
		 // "Screen Boundary: %.2f, %.2f - %.2f, %.2f  Tablet Boundary: %.2f, %.2f - %.2f, %.2f  Ratios H/V/X: %.2f %.2f %.2f\nOld Pos: %.2f %.2f  | Scr Pos: %.2f %.2f  |  Motion: %d %d\n\n"
		 fprintf(stderr,
		 "Screen Boundary: %.2f, %.2f - %.2f, %.2f  Tablet Boundary: %.2f, %.2f - %.2f, %.2f  Ratios H/V/X: %.2f %.2f %d\n",
		 sx1, sy1, sx2, sy2,
		 tx1, ty1, tx2, ty2,
		 hratio, vratio, mouse_scaling);
		
		 		
		// Apply the tablet:screen ratio to the amount of motion
		// (because it's usually a sane value)
		//
		// TODO: Replace with actual mouse acceleration.
		//
		nx = stylus.oldPos.x - screenBounds.origin.x + stylus.motion.x * mouse_scaling * minratio;
		ny = stylus.oldPos.y - screenBounds.origin.y + stylus.motion.y * mouse_scaling * minratio;
		
		// In mouse mode limit motion to the designated screen bounds
		if (nx < 0) nx = 0;
		if (nx >= swide) nx = swide - 1;
		if (ny < 0) ny = 0;
		if (ny >= shigh) ny = shigh - 1;
		
		stylus.scrPos.x = (SInt16)nx + screenBounds.origin.x;
		stylus.scrPos.y = (SInt16)ny + screenBounds.origin.y;
		
		fprintf(stderr,  "Current Pos: %d %d | Old Pos: %d %d | Motion: %d %d | Scr Pos: %.2f %.2f  |  Old Scr Pos: %.2f %.2f\n\n",
				stylus.point.x, stylus.point.y,
				stylus.old.x,stylus.old.y,
				stylus.motion.x, stylus.motion.y,
				stylus.oldPos.x, stylus.oldPos.y,
				stylus.scrPos.x, stylus.scrPos.y);
		
		/**/
		
	}
	else {
		
		// Get the ratio of the screen to the tablet
		CGFloat hratio = swide / twide, vratio = shigh / thigh;
		
		// Constrain the stylus to the active tablet area
		CGFloat x = stylus.point.x, y = stylus.point.y;
		
		if (x < tx1)  x = tx1;
		if (x > tx2)  x = tx2;
		if (y < ty1)  y = ty1;
		if (y > ty2)  y = ty2;
		
		// Map the Stylus Point to the active Screen Area
		nx = (sx1 + (x - tx1) * hratio);
		ny = (sy1 + (y - ty1) * vratio);
		
		stylus.scrPos.x = (SInt16)(nx + screenBounds.origin.x);
		stylus.scrPos.y = (SInt16)(ny + screenBounds.origin.y);
// #define DEBUG
		// "Screen Boundary: %.2f, %.2f - %.2f, %.2f  Tablet Boundary: %.2f, %.2f - %.2f, %.2f  Ratios H/V/X: %.2f %.2f %.2f\nOld Pos: %.2f %.2f  | Scr Pos: %.2f %.2f  |  Motion: %d %d\n\n"
#ifdef DEBUG
		fprintf(stderr,
				"Screen Boundary: %.2f, %.2f - %.2f, %.2f  Tablet Boundary: %.2f, %.2f - %.2f, %.2f  Ratios H/V/X: %.2f %.2f %d\n",
				sx1, sy1, sx2, sy2,
				tx1, ty1, tx2, ty2,
				hratio, vratio, mouse_scaling);
		
		fprintf(stderr,  "Current Pos: %d %d | Old Pos: %d %d | Motion: %d %d | Scr Pos: %.2f %.2f  |  Old Scr Pos: %.2f %.2f\n\n",
				stylus.point.x, stylus.point.y,
				stylus.old.x,stylus.old.y,
				stylus.motion.x, stylus.motion.y,
				stylus.oldPos.x, stylus.oldPos.y,
				stylus.scrPos.x, stylus.scrPos.y);
#endif		
		
				/**/
		
	}
	
	//
	// Map Stylus buttons to system buttons
	//
	bzero(buttonState, sizeof(buttonState));
	buttonState[button_mapping[kStylusTip]]		|= stylus.button[kStylusTip];
	
	buttonState[button_mapping[kStylusButton1]]	|= stylus.button[kStylusButton1];
	buttonState[button_mapping[kStylusButton2]]	|= stylus.button[kStylusButton2];
	buttonState[button_mapping[kStylusEraser]]	|= stylus.button[kStylusEraser];
	
	int buttonEvent = (dragState || buttonState[kSystemClickOrRelease]  || buttonState[kSystemButton1] || buttonState[kSystemEraser]) ? NX_LMOUSEDRAGGED : (buttonState[kSystemButton2] ? NX_RMOUSEDRAGGED : NX_MOUSEMOVED);
	
	//
	// TODO: Support eraser-via-button by sending a stream of events:
	//
	// 1. Current button up
	// 2. Current tip-type exit proximity
	// 3. Eraser enter proximity
	// 4. Eraser down
	//
	
	bool postedPosition = false;
	
	// Has the stylus moved in or out of range?
	if (oldStylus.off_tablet != stylus.off_tablet) {
		if ((stylus.proximity.enterProximity = !stylus.off_tablet))
			stylus.proximity.pointerType = (stylus.eraser_flag && (button_mapping[kStylusEraser] == kSystemEraser)) ? EEraser : EPen;
		PostNXEvent(buttonEvent, NX_SUBTYPE_TABLET_PROXIMITY,0);
		fprintf(stderr, "Stylus has %s proximity\n", stylus.off_tablet ? "exited" : "entered");
	}
	
	// Is a Double-Click warranted?
	if (buttonState[kSystemDoubleClick] && !oldButtonState[kSystemDoubleClick]) {
		if (oldButtonState[kSystemButton1]) {
			PostNXEvent(NX_LMOUSEUP, NX_SUBTYPE_TABLET_POINT,0);
			ShortSleep();
		}
		
		PostNXEvent(NX_LMOUSEDOWN, NX_SUBTYPE_TABLET_POINT,0);
		ShortSleep();
		PostNXEvent(NX_LMOUSEUP, NX_SUBTYPE_TABLET_POINT,0);
		ShortSleep();
		PostNXEvent(NX_LMOUSEDOWN, NX_SUBTYPE_TABLET_POINT,0);
		
		if (!oldButtonState[kSystemButton1]) {
			ShortSleep();
			PostNXEvent(NX_LMOUSEUP, NX_SUBTYPE_TABLET_POINT,0);
		}
		
		postedPosition = true;
	}
	
	// Is a Single-Click warranted?	
	if (buttonState[kSystemSingleClick] && !oldButtonState[kSystemSingleClick]) {
		if (oldButtonState[kSystemButton1]) {
			PostNXEvent(NX_LMOUSEUP, NX_SUBTYPE_TABLET_POINT,0);
			ShortSleep();
		}
		
		PostNXEvent(NX_LMOUSEDOWN, NX_SUBTYPE_TABLET_POINT,0);
		ShortSleep();
		PostNXEvent(NX_LMOUSEUP, NX_SUBTYPE_TABLET_POINT,0);
		
		if (!oldButtonState[kSystemButton1]) {
			ShortSleep();
			PostNXEvent(NX_LMOUSEUP, NX_SUBTYPE_TABLET_POINT,0);
		}
		
		postedPosition = true;
	}
	 
	// Is this a Grab or Drop ?
	if (!buttonState[kSystemClickOrRelease] && oldButtonState[kSystemClickOrRelease]) {
		dragState = !dragState;

		
		if (!dragState || !buttonState[kSystemButton1]) {
			PostNXEvent((dragState ? NX_LMOUSEDOWN : NX_LMOUSEUP), NX_SUBTYPE_TABLET_POINT,0);
			postedPosition = true;
			fprintf(stderr, "Drag %sed\n", dragState ? "Start" : "End");
		}
	}
	
	// Has Button 1 changed?
	if (oldButtonState[kSystemButton1] != buttonState[kSystemButton1]) {
		if (dragState && !buttonState[kSystemButton1]) {
			dragState = false;
			fprintf(stderr, "Drag Canceled\n");
		}
		
		if (!dragState) {
			PostNXEvent((buttonState[kSystemButton1] ? NX_LMOUSEDOWN : NX_LMOUSEUP), NX_SUBTYPE_TABLET_POINT,0);
			postedPosition = true;
		}
	}
	
	// Has Button 2 changed?
	if (oldButtonState[kSystemButton2] != buttonState[kSystemButton2]) {
		PostNXEvent((buttonState[kSystemButton2] ? NX_RMOUSEDOWN : NX_RMOUSEUP), NX_SUBTYPE_TABLET_POINT,0);
		postedPosition = true;
	}
	
	// Has the Eraser changed?
	if (oldButtonState[kSystemEraser] != buttonState[kSystemEraser]) {
		PostNXEvent((buttonState[kSystemEraser] ? NX_LMOUSEDOWN : NX_LMOUSEUP), NX_SUBTYPE_TABLET_POINT,0);
		postedPosition = true;
	}
	
	// Has Button 3 changed?
	if (oldButtonState[kSystemButton3] != buttonState[kSystemButton3])
		PostNXEvent((buttonState[kSystemButton3] ? NX_OMOUSEDOWN : NX_OMOUSEUP), NX_SUBTYPE_DEFAULT, kOtherButton3);
	
	// Has Button 4 changed?
	if (oldButtonState[kSystemButton4] != buttonState[kSystemButton4])
		PostNXEvent((buttonState[kSystemButton4] ? NX_OMOUSEDOWN : NX_OMOUSEUP), NX_SUBTYPE_DEFAULT, kOtherButton4);
	
	// Has Button 5 changed?
	if (oldButtonState[kSystemButton5] != buttonState[kSystemButton5])
		PostNXEvent((buttonState[kSystemButton5] ? NX_OMOUSEDOWN : NX_OMOUSEUP), NX_SUBTYPE_DEFAULT, kOtherButton5);
	
	// Has the stylus changed position?
	
	if (!postedPosition && (oldStylus.point.x != stylus.point.x || oldStylus.point.y != stylus.point.y))
		PostNXEvent(buttonEvent, NX_SUBTYPE_TABLET_POINT,0);
	
	// Finally, remember the current state for next time
	bcopy(&stylus, &oldStylus, sizeof(stylus));
	bcopy(&buttonState, &oldButtonState, sizeof(buttonState));
}


void PostNXEvent(int eventType, SInt16 eventSubType, UInt8 otherButton) {
	if (!tablet_on)
		return;
	
	static NXEventData eventData;
	
#if LOG_STREAM_TO_FILE
	if (logfile) fprintf(logfile, " | PostNXEvent(%d, %d, %02X)", eventType, eventSubType, otherButton);
#endif
	
	switch (eventType) {
		case NX_OMOUSEUP:
		case NX_OMOUSEDOWN:
			eventData.mouse.click = 0;
			eventData.mouse.buttonNumber = otherButton;
			
#if LOG_STREAM_TO_FILE
			if (logfile) fprintf(logfile, " (other button)");
#endif
			break;
			
		case NX_LMOUSEUP:
		case NX_LMOUSEDOWN:
		case NX_RMOUSEUP:
		case NX_RMOUSEDOWN:
			
			//			if (!no_tablet_events) {
			//				fprintf(output, "[POST] Button Event %d\n", eventType);
			
			eventData.mouse.subType = eventSubType;
			eventData.mouse.subx = 0;
			eventData.mouse.suby = 0;
			eventData.mouse.pressure = stylus.pressure; 
			
#if LOG_STREAM_TO_FILE
			if (logfile) fprintf(logfile, " | UP/DOWN | pressure=%u", stylus.pressure);
#endif
			
			//				/* SInt16 */	eventData.mouse.eventNum = 1;		/* unique identifier for this button */
			/* SInt32 */	eventData.mouse.click = 0;			/* click state of this event */
			//				/* UInt8 */		eventData.mouse.buttonNumber = 1;	/* button generating other button event (0-31) */
			/* UInt8 */		eventData.mouse.reserved2 = 0;
			/* SInt32 */	eventData.mouse.reserved3 = 0;
			
			switch (eventSubType) {
				case NX_SUBTYPE_TABLET_POINT:
					eventData.mouse.tablet.point.x = stylus.point.x;
					eventData.mouse.tablet.point.y = stylus.point.y;
					eventData.mouse.tablet.point.buttons = 0x0000;
					eventData.mouse.tablet.point.tilt.x = stylus.tilt.x;
					eventData.mouse.tablet.point.tilt.y = stylus.tilt.y;
					eventData.mouse.tablet.point.deviceID = stylus.proximity.deviceID;
					
#if LOG_STREAM_TO_FILE
					if (logfile) fprintf(logfile, " | point=(%d,%d) | tilt=(%d,%d)", stylus.point.x, stylus.point.y, stylus.tilt.x, stylus.tilt.y);
#endif
					
					/* SInt32 */ eventData.mouse.tablet.point.z = 0;					/* absolute z coordinate in tablet space at full tablet resolution */
					/* UInt16 */ eventData.mouse.tablet.point.pressure = stylus.pressure;				/* scaled pressure value; MAX=(2^16)-1, MIN=0 */
					/* UInt16 */ eventData.mouse.tablet.point.rotation = 0;				/* Fixed-point representation of device rotation in a 10.6 format */
					/* SInt16 */ eventData.mouse.tablet.point.tangentialPressure = 0;	/* tangential pressure on the device; same range as tilt */
					//						/* SInt16 */ eventData.mouse.tablet.point.vendor1 = 0;				/* vendor-defined signed 16-bit integer */
					//						/* SInt16 */ eventData.mouse.tablet.point.vendor2 = 0;				/* vendor-defined signed 16-bit integer */
					//						/* SInt16 */ eventData.mouse.tablet.point.vendor3 = 0;				/* vendor-defined signed 16-bit integer */
					break;
					
				case NX_SUBTYPE_TABLET_PROXIMITY:
//					bcopy(&stylus.proximity, &eventData.mouse.tablet.proximity, sizeof(stylus.proximity));
#if LOG_STREAM_TO_FILE
					if (logfile) fprintf(logfile, " | PROXIMITY");
#endif
					break;
			}
			//			}
			break;
			
		case NX_MOUSEMOVED:
		case NX_LMOUSEDRAGGED:
		case NX_RMOUSEDRAGGED:
			
			//			if (!no_tablet_events) {
			//				fprintf(output, "[POST] Mouse Event %d Subtype %d\n", eventType, eventSubType);
			
			eventData.mouseMove.subType = eventSubType;
			/* UInt8 */		eventData.mouseMove.reserved1 = 0;
			/* SInt32 */	eventData.mouseMove.reserved2 = 0;
			
			switch (eventSubType) {
				case NX_SUBTYPE_TABLET_POINT:
					eventData.mouseMove.tablet.point.x = stylus.point.x;
					eventData.mouseMove.tablet.point.y = stylus.point.y;
					eventData.mouseMove.tablet.point.buttons = 0x0000;
					eventData.mouseMove.tablet.point.pressure = stylus.pressure;
					eventData.mouseMove.tablet.point.tilt.x = stylus.tilt.x;
					eventData.mouseMove.tablet.point.tilt.y = stylus.tilt.y;
					eventData.mouseMove.tablet.point.deviceID = stylus.proximity.deviceID;
					
#if LOG_STREAM_TO_FILE
					if (logfile) fprintf(logfile, " | MOVE | pressure=%u | point=(%d,%d) | tilt=(%d,%d)", stylus.pressure, stylus.point.x, stylus.point.y, stylus.tilt.x, stylus.tilt.y);
#endif
					
					/* SInt32 */ eventData.mouseMove.tablet.point.z = 0;					/* absolute z coordinate in tablet space at full tablet resolution */
					/* UInt16 */ eventData.mouseMove.tablet.point.rotation = 0;				/* Fixed-point representation of device rotation in a 10.6 format */
					/* SInt16 */ eventData.mouseMove.tablet.point.tangentialPressure = 0;	/* tangential pressure on the device; same range as tilt */
					//						/* SInt16 */ eventData.mouseMove.tablet.point.vendor1 = 0;				/* vendor-defined signed 16-bit integer */
					//						/* SInt16 */ eventData.mouseMove.tablet.point.vendor2 = 0;				/* vendor-defined signed 16-bit integer */
					//						/* SInt16 */ eventData.mouseMove.tablet.point.vendor3 = 0;				/* vendor-defined signed 16-bit integer */
					break;
					
				case NX_SUBTYPE_TABLET_PROXIMITY:
//					bcopy(&stylus.proximity, &eventData.mouseMove.tablet.proximity, sizeof(NXTabletProximityData));
#if LOG_STREAM_TO_FILE
					if (logfile) fprintf(logfile, " | PROXIMITY");
#endif
					break;
			}
			//			}
			
			// Relative motion is needed for the mouseMove event
			if (stylus.oldPos.x == SHRT_MIN) {
				eventData.mouseMove.dx = eventData.mouseMove.dy = 0;
			}
			else {
				eventData.mouseMove.dx = (SInt32)(stylus.scrPos.x - stylus.oldPos.x);
				eventData.mouseMove.dy = (SInt32)(stylus.scrPos.y - stylus.oldPos.y);
			}
			eventData.mouseMove.subx = 0;
			eventData.mouseMove.suby = 0;
			stylus.oldPos = stylus.scrPos;
#if LOG_STREAM_TO_FILE
			if (logfile) fprintf(logfile, " | delta=(%d,%d)", eventData.mouseMove.dx, eventData.mouseMove.dy);
#endif
			
			break;
	}
	
	// Generate the tablet event to the system event driver
	IOGPoint newPoint = { stylus.scrPos.x, stylus.scrPos.y };
	
	(void)IOHIDPostEvent(gEventDriver, eventType, newPoint, &eventData, kNXEventDataVersion, 0, kIOHIDSetCursorPosition);
	
#if LOG_STREAM_TO_FILE
	if (logfile) fprintf(logfile, " | xy=(%.2f,%.2f)", stylus.scrPos.x, stylus.scrPos.y);
#endif
	
	//	if (!no_tablet_events) {
	//
	// Some apps only expect proximity events to arrive as pure tablet events (Desktastic, for one).
	// Generate a pure tablet form of all proximity events as well.
	//
	if (eventSubType == NX_SUBTYPE_TABLET_PROXIMITY) {
		//			fprintf(output, "[POST] Proximity Event %d Subtype %d\n", NX_TABLETPROXIMITY, NX_SUBTYPE_TABLET_PROXIMITY);
		bcopy(&stylus.proximity, &eventData.proximity, sizeof(NXTabletProximityData));
		(void)IOHIDPostEvent(gEventDriver, NX_TABLETPROXIMITY, newPoint, &eventData, kNXEventDataVersion, 0, 0);
	}
	//	}
}

//
// CloseHIDService
//
kern_return_t CloseHIDService() {
	kern_return_t   r = KERN_SUCCESS;
	
	if (gEventDriver != MACH_PORT_NULL)
		r = IOServiceClose(gEventDriver);
	
	gEventDriver = MACH_PORT_NULL;
	return r;
}

//
// OpenHidService
//
kern_return_t OpenHIDService() {
	kern_return_t   kr;
	mach_port_t		ev, service;
	
	if (KERN_SUCCESS == (kr = CloseHIDService())) {
		if (KERN_SUCCESS == (kr = IOMasterPort(MACH_PORT_NULL, &io_master_port)) && io_master_port != MACH_PORT_NULL) {
			if ((service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(kIOHIDSystemClass)))) {
				kr = IOServiceOpen(service, mach_task_self(), kIOHIDParamConnectType, &ev);
				IOObjectRelease(service);
				
				if (KERN_SUCCESS == kr)
					gEventDriver = ev;
			}
		}
	}
	
	return kr;
}


void InitStylus() {
	stylus.toolid		= kToolPen1;
	stylus.tool			= kToolTypePen;
	stylus.serialno		= 0;
	
	stylus.off_tablet	= true;
	stylus.pen_near		= false;
	stylus.eraser_flag	= false;
	
	stylus.button_click	= false;
	ResetButtons;
	
	stylus.menu_button	= 0;
	stylus.raw_pressure	= 0;
	stylus.pressure		= 0;
	stylus.tilt.x		= 0;
	stylus.tilt.y		= 0;
	stylus.point.x		= 0;
	stylus.point.y		= 0;
	
	CGEventRef ourEvent = CGEventCreate(NULL);
	CGPoint point = CGEventGetLocation(ourEvent);
	
	stylus.scrPos		= point;
	stylus.oldPos.x		= SHRT_MIN;
	stylus.oldPos.y		= SHRT_MIN;
	
	// The proximity record includes these identifiers
	stylus.proximity.vendorID = 0xBEEF;				// A made-up Vendor ID (Wacom's is 0x056A)
	stylus.proximity.tabletID = 0x0001;
	stylus.proximity.deviceID = 0x81;				// just a single device for now
	stylus.proximity.pointerID = 0x00;
	stylus.proximity.systemTabletID = 0x00;
	stylus.proximity.vendorPointerType = 0x0802;	// basic stylus
	stylus.proximity.pointerSerialNumber = 0x00000001;
	stylus.proximity.reserved1 = 0;
	
	// This will be replaced when a tablet is located
	stylus.proximity.uniqueID = 0;
	
	// Indicate which fields in the point event contain valid data. This allows
	// applications to handle devices with varying capabilities.
	
	stylus.proximity.capabilityMask =
	NX_TABLET_CAPABILITY_DEVICEIDMASK
	|	NX_TABLET_CAPABILITY_ABSXMASK
	|	NX_TABLET_CAPABILITY_ABSYMASK
	//		|	NX_TABLET_CAPABILITY_VENDOR1MASK
	//		|	NX_TABLET_CAPABILITY_VENDOR2MASK
	//		|	NX_TABLET_CAPABILITY_VENDOR3MASK
	|	NX_TABLET_CAPABILITY_BUTTONSMASK
	|	NX_TABLET_CAPABILITY_TILTXMASK
	|	NX_TABLET_CAPABILITY_TILTYMASK
	//		|	NX_TABLET_CAPABILITY_ABSZMASK
	|	NX_TABLET_CAPABILITY_PRESSUREMASK
	//		|	NX_TABLET_CAPABILITY_TANGENTIALPRESSUREMASK
	//		|	NX_TABLET_CAPABILITY_ORIENTINFOMASK
	//		|	NX_TABLET_CAPABILITY_ROTATIONMASK
	;
	
	/*
	 //
	 // Use Wacom-supplied names
	 //
	 stylus.proximity.capabilityMask =	kTransducerAbsXBitMask
	 | kTransducerAbsYBitMask
	 | kTransducerButtonsBitMask
	 | kTransducerTiltXBitMask
	 | kTransducerTiltYBitMask
	 | kTransducerDeviceIdBitMask	// no use for this today
	 | kTransducerPressureBitMask;
	 
	 */
	
	bcopy(&stylus, &oldStylus, sizeof(StylusState));
	bzero(buttonState, sizeof(buttonState));
	bzero(oldButtonState, sizeof(oldButtonState));
}


//
// ResetStylus()
//	Simulate as if the pen were taken off the tablet
//
void ResetStylus() {
	stylus.off_tablet	= true;
	stylus.pen_near		= false;
	stylus.eraser_flag	= false;
	
	ResetButtons;
	
	stylus.menu_button	= 0;
	stylus.pressure		= 0;
	stylus.tilt.x		= 0;
	stylus.tilt.y		= 0;
	
	PostChangeEvents();
}

// <End of TabletMagic Code>


// 
void print_help(int exval) {
	printf("%s [-v ID] [-p ID] [-w WIDTH] [-h HEIGHT] [-x X-OFFSET] [-y Y-OFFSET]\n\n", "hyperpenDaemon");
	
	printf("  -v ID           set vendor  (e.g. 0x08ca)\n");
	printf("  -p ID           set product (e.g. 0x0010)\n");

	printf("  -w WIDTH        set tablet width  (e.g. 6000)\n");
	printf("  -h HEIGHT       set tablet height (e.g. 4500)\n");

	printf("  -x X-OFFSET     set tablet x offset (e.g. 0)\n");
	printf("  -y Y-OFFSET     set tablet y offset (e.g. 0)\n\n");
	
	exit(exval);
}
// the main as all other code is naive because it assumes everything
// runs well - allthough every call returns a status code I don't handle
// them at the moment
//
// need to add options to set tablet make and attributes
// will setup profiles that can be loaded for the function keys
//
int main (int argc, char * argv[]) {
	
	fprintf(stderr, "Aiptek Tablet Driver for OSX\nDesigned and tested for HyperPen 12000U\n(c) Udo Killermann 2011\n\n");
	
	IOHIDManagerRef	ioHidManager;
	IOReturn ioReturn;
	
	CFMutableDictionaryRef matchingDictionary;

	CFNumberRef vendorID;
	CFNumberRef productID;
	
	
	UInt32 vendor;
	UInt32 product;
	int opt;
	int width=6000;
	int height=4500;
	int offsetX=0;
	int offsetY=0;

// Aiptek HyperPen 12000U
// taken from the top defines
	
	product=ProductID;
	vendor=VendorID;
	

	while((opt = getopt(argc, argv, "v:p:w:h:x:y:o")) != -1) {
		switch(opt) {
			case 'o':
				print_help(0);
				exit(0);
				break;
			case 'v':
				vendor=strtol(optarg,(char **)NULL, 16);
				break;
			case 'p':
				product=strtol(optarg,(char **)NULL, 16);
				break;
			case 'w':
				width=atoi(optarg);
				break;
			case 'h':
				height=atoi(optarg);
				break;
			case 'x':
				offsetX=atoi(optarg);
				break;
			case 'y':
				offsetY=atoi(optarg);
				break;
			case ':':
				fprintf(stderr, "Error - Option `%c' needs a value\n\n", optopt);
				print_help(1);
				break;
			case '?':
				fprintf(stderr, "Error - No such option: `%c'\n\n", optopt);
				print_help(1);
		}
	}
	
	printf("vendor: %x\n",vendor);
	printf("product: %x\n",product);
	printf("width: %d\n", width);
	printf("height: %d\n", height);
	printf("Offset X: %d\n",offsetX);
	printf("Offset Y: %d\n",offsetY);
	
	
	ioHidManager=IOHIDManagerCreate(kIOHIDOptionsTypeNone,0);
	
	vendorID=CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vendor);
	productID=CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &product);

	// Create Matching dictionary	
	matchingDictionary=CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	// set entries
	CFDictionarySetValue(matchingDictionary, CFSTR(kIOHIDVendorIDKey), vendorID);
	CFDictionarySetValue(matchingDictionary, CFSTR(kIOHIDProductKey), productID);
	
	CFRelease(productID);
	CFRelease(vendorID);

	IOHIDManagerSetDeviceMatching(ioHidManager, matchingDictionary);
	// to support other Aiptek Tablets and Medion clones I'll have to use
	// IOHIDManagerSetDeviceMatchingMultiple and deliver distinct dictionaries
	// for every device to match
	
	ioReturn=IOHIDManagerOpen(ioHidManager,kIOHIDOptionsTypeSeizeDevice);
	
	if (ioReturn==kIOReturnSuccess) {
		IOHIDManagerRegisterDeviceRemovalCallback(ioHidManager, theDeviceRemovalCallback, NULL) ; 
		IOHIDManagerRegisterDeviceMatchingCallback(ioHidManager, theDeviceMatchingCallback,NULL);
		
		IOHIDManagerScheduleWithRunLoop( ioHidManager , CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
		
		
		ioReturn=OpenHIDService();
		InitStylus();
		
		// hard coded for Aiptek 12000U at the moment - has to be removed by the resolution of
		// the connected device
		
		InitTabletBounds(offsetX, offsetY, width, height);
		UpdateDisplaysBounds();
		SetScreenMapping(0,0,-1,-1);
		
		CFRunLoopRun();
		
		ioReturn=CloseHIDService();
		
		ioReturn=IOHIDManagerClose(ioHidManager , kIOHIDOptionsTypeNone);
		
		CFRelease(matchingDictionary);
		CFRelease(ioHidManager);
		
	}
  
    return 0;
}

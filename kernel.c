static char RCSId[] = "$Id: kernel.c,v 1.2 1993/07/04 04:04:21 root Exp root $";
static char Copyright[] = "Copyright  Robert J. Amstadt, 1993";

#include <stdio.h>
#include <stdlib.h>
#include "prototypes.h"

extern unsigned short *Stack16Frame;

/**********************************************************************
 *					KERNEL_LockSegment
 */
int
KERNEL_LockSegment(int segment)
{
    if (segment == -1)
	segment = *(Stack16Frame + 6);

#ifdef RELAY_DEBUG
    printf("LockSegment: segment %x\n", segment);
#endif

    return segment;
}

/**********************************************************************
 *					KERNEL_UnlockSegment
 */
int
KERNEL_UnlockSegment(int segment)
{
    if (segment == -1)
	segment = *(Stack16Frame + 6);

#ifdef RELAY_DEBUG
    printf("UnlockSegment: segment %x\n", segment);
#endif

    return segment;
}

/**********************************************************************
 *					KERNEL_WaitEvent
 */
int
KERNEL_WaitEvent(int task)
{
#ifdef RELAY_DEBUG
    printf("WaitEvent: task %d\n", task);
#endif
    return 0;
}
/**********************************************************************
 *					KERNEL_GetModuleFileName
 */
int
KERNEL_GetModuleFileName(int module, char *filename, int bytes)
{
#ifdef RELAY_DEBUG
    printf("GetModuleFileName: module %d, filename %x, bytes %d\n", 
	    module, filename, bytes);
#endif
    
    strcpy(filename, "TEST.EXE");
    
    return strlen(filename);
}

/**********************************************************************
 *					KERNEL_DOS3Call
 */
int
KERNEL_DOS3Call(int ax, int cx, int dx, int bx, int sp, int bp,
		int si, int di, int ds, int es)
{
    switch ((ax >> 8) & 0xff)
    {
      case 0x30:
	return 0x0303;
	
      case 0x25:
      case 0x35:
	return 0;

      case 0x4c:
	exit(ax & 0xff);

      default:
	fprintf(stderr, "DOS: AX %04x, BX %04x, CX %04x, DX %04x\n",
		ax, bx, cx, dx);
	fprintf(stderr, "     SP %04x, BP %04x, SI %04x, DI %04x\n",
		sp, bp, si, di);
	fprintf(stderr, "     DS %04x, ES %04x\n",
		ds, es);
    }
    
    return 0;
}

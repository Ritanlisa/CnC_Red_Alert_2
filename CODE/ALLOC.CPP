/*
**	Command & Conquer Red Alert(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/***************************************************************************
 **   C O N F I D E N T I A L --- W E S T W O O D   A S S O C I A T E S   **
 ***************************************************************************
 *                                                                         *
 *                 Project Name : Westwood Library                         *
 *                                                                         *
 *                    File Name : ALLOC.CPP                                *
 *                                                                         *
 *                   Programmer : Joe L. Bostic                            *
 *                                                                         *
 *                   Start Date : February 1, 1992                         *
 *                                                                         *
 *                  Last Update : March 9, 1995 [JLB]                      *
 *                                                                         *
 *-------------------------------------------------------------------------*
 * Functions:                                                              *
 *   Alloc -- Allocates system RAM.                                        *
 *   Ram_Free -- Determines the largest free chunk of RAM.                 *
 *   Free -- Free an Alloc'ed block of RAM.                                *
 *   Resize_Alloc -- Change the size of an allocated block.                *
 *   Heap_Size -- Size of the heap we have.                                *
 *   Total_Ram_Free -- Total amount of free RAM.                           *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

// #include <malloc.h>
// #include <string.h>
// #include <stdlib.h>
// #include <dos.h>
// #include <bios.h>
// #include <stdio.h>

#include <memory>
#include <mutex>
#include <vector>
#include <stdexcept>
#include <system_error>
#include <algorithm>

#ifdef WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifndef WWMEM_H
#include "wwmem.h"
#endif

// extern "C" unsigned long Largest_Mem_Block ( void ) ;

//
// use double-word alignment for allocs
//
// #define LONG_ALIGNMENT			1

/*
** Define the equates necessary to call a DPMI interrupt.
*/
// #define	DPMI_INT									0x0031
// #define	DPMI_LOCK_MEM							0x0600
// #define	DPMI_UNLOCK_MEM						0x0601
// #define  LOGGING									FALSE
/*=========================================================================*/
/* The following PRIVATE functions are in this file:                       */
/*=========================================================================*/

/*= = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =*/

// unsigned long MinRam=0L;		// Record of least memory at worst case.
// unsigned long MaxRam=0L;		// Record of total allocated at worst case.
// static unsigned long TotalRam = 0L;
// static unsigned long Memory_Calls = 0L;
// static unsigned long RequestedSystemRam = 8*1024*1024;
// static unsigned long LargestRamBlock = 0L;
//
// void (*Memory_Error)(void) = NULL;
// void (*Memory_Error_Exit)(char *string) = NULL;

// 内存类型标记（兼容原始定义）
enum MemoryFlagType
{
	MEM_NORMAL = 0,
	MEM_CLEAR = 1 << 0,
	MEM_LOCK = 1 << 1,
	MEM_REAL = 1 << 2
};

// 内存块元数据（兼容原始头信息）
#pragma pack(push, 1)
struct BlockMeta
{
	size_t size;		  // 用户请求大小
	MemoryFlagType flags; // 分配标记
	uint32_t checksum;	  // 数据校验
#if defined(_DEBUG)
	uint64_t magic = 0xDEADBEEFCAFEBABE; // 调试魔数
#endif
};
#pragma pack(pop)

// 全局内存追踪系统
namespace MemorySystem
{
	std::mutex allocationMutex;
	std::atomic<size_t> totalAllocated{0};
	std::atomic<size_t> peakAllocated{0};
	std::atomic<size_t> allocationCount{0};

	// 内存块验证函数
	void ValidateBlock(const BlockMeta *meta)
	{
#if defined(_DEBUG)
		if (meta->magic != 0xDEADBEEFCAFEBABE)
		{
			throw std::runtime_error("Memory corruption detected");
		}
#endif
		// 校验和验证
		const uint32_t stored = meta->checksum;
		const uint32_t calculated = static_cast<uint32_t>(meta->size ^ meta->flags);
		if (stored != calculated)
		{
			throw std::runtime_error("Memory checksum mismatch");
		}
	}

	// 内存锁定函数
	void LockMemory(void *ptr, size_t size)
	{
#ifdef WIN32
		if (!VirtualLock(ptr, size))
		{
			throw std::system_error(GetLastError(), std::system_category(), "VirtualLock failed");
		}
#else
		if (mlock(ptr, size) != 0)
		{
			throw std::system_error(errno, std::system_category(), "mlock failed");
		}
#endif
	}

	// 内存解锁函数
	void UnlockMemory(void *ptr, size_t size)
	{
#ifdef WIN32
		VirtualUnlock(ptr, size);
#else
		munlock(ptr, size);
#endif
	}
}

// 现代内存分配实现
void *ModernAlloc(size_t size, MemoryFlagType flags = MEM_NORMAL)
{
	const size_t headerSize = sizeof(BlockMeta);
	const size_t totalSize = headerSize + size;

	// 内存分配
	void *basePtr = nullptr;
#ifdef WIN32
	basePtr = VirtualAlloc(nullptr, totalSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
	basePtr = aligned_alloc(sysconf(_SC_PAGESIZE), totalSize);
#endif

	if (!basePtr)
		throw std::bad_alloc();

	// 初始化元数据
	BlockMeta *meta = static_cast<BlockMeta *>(basePtr);
	meta->size = size;
	meta->flags = flags;
	meta->checksum = static_cast<uint32_t>(size ^ flags);

	// 内存锁定处理
	if (flags & MEM_LOCK)
	{
		try
		{
			MemorySystem::LockMemory(basePtr, totalSize);
		}
		catch (...)
		{
#ifdef WIN32
			VirtualFree(basePtr, 0, MEM_RELEASE);
#else
			free(basePtr);
#endif
			throw;
		}
	}

	// 内存清零
	void *userPtr = reinterpret_cast<uint8_t *>(basePtr) + headerSize;
	if (flags & MEM_CLEAR)
	{
		memset(userPtr, 0, size);
	}

	// 更新追踪数据
	MemorySystem::totalAllocated += totalSize;
	MemorySystem::peakAllocated = max(
		MemorySystem::peakAllocated.load(),
		MemorySystem::totalAllocated.load());
	MemorySystem::allocationCount++;

	return userPtr;
}

/***************************************************************************
 * DPMI_LOCK -- handles locking a block of DPMI memory                     *
 *                                                                         *
 * INPUT:                                                                  *
 *                                                                         *
 * OUTPUT:                                                                 *
 *                                                                         *
 * WARNINGS:                                                               *
 *                                                                         *
 * HISTORY:                                                                *
 *   06/23/1995 PWG : Created.                                             *
 *=========================================================================*/
#include "mono.h"
#include <iostream>
void DPMI_Lock(VOID const *ptr, long const size)
{
	//	union REGS regs;
	//	struct SREGS sregs;
	//
	//	/*
	//	** Lock memory
	//	** AX = 0x600
	//	** BX:CX = starting linear address of memory to lock
	//	** SI:DI = size of region to lock (in bytes)
	//	** - If Failure, carry flag is set.
	//	*/
	//	memset (&regs, 0 ,sizeof(regs));
	//	segread (&sregs);
	//	regs.x.eax = DPMI_LOCK_MEM;
	//	regs.x.ebx = ((long)ptr & 0xffff0000) >> 16;
	//	regs.x.ecx = ((long)ptr & 0x0000ffff);
	//	regs.x.esi = ((long)size & 0xffff0000) >> 16;
	//	regs.x.edi = ((long)size & 0x0000ffff);
	//	int386x (DPMI_INT, &regs, &regs, &sregs);			// call DPMI
	////	if (regs.x.cflag) {
	////	}
	// #if(0)
	//	char *temp = (char *)ptr;
	//	char hold;
	//	for (int lp = 0; lp < size; lp += 2048) {
	//		hold = *temp;
	//		temp += 2048;
	//	}
	// #endif
	MemorySystem::LockMemory((void *)ptr, size);
}

/***************************************************************************
 * DPMI_UNLOCK -- Handles unlocking a locked block of DPMI                 *
 *                                                                         *
 * INPUT:                                                                  *
 *                                                                         *
 * OUTPUT:                                                                 *
 *                                                                         *
 * WARNINGS:                                                               *
 *                                                                         *
 * HISTORY:                                                                *
 *   06/23/1995 PWG : Created.                                             *
 *=========================================================================*/
void DPMI_Unlock(void const *ptr, long const size)
{
	//	union REGS regs;
	//	struct SREGS sregs;
	//
	//	/*
	//	** Unlock the memory
	//	*/
	//	memset (&regs, 0 ,sizeof(regs));
	//	segread (&sregs);
	//	regs.x.eax = DPMI_UNLOCK_MEM;							// DPMI function to call
	//	regs.x.ebx = ((long)ptr & 0xffff0000) >> 16;
	//	regs.x.ecx = ((long)ptr & 0x0000ffff);
	//	regs.x.esi = ((long)size & 0xffff0000) >> 16;
	//	regs.x.edi = ((long)size & 0x0000ffff);
	//	int386x (DPMI_INT, &regs, &regs, &sregs);			// call DPMI
	////	if (regs.x.cflag) {
	////	}
	MemorySystem::UnlockMemory((void *)ptr, size);
}

/***************************************************************************
 * Alloc -- Allocates system RAM.                                          *
 *                                                                         *
 *    This is the basic RAM allocation function.  It is used for all       *
 *    memory allocations needed by the system or the main program.         *
 *                                                                         *
 * INPUT:   bytes_to_alloc -- LONG value of the number of bytes to alloc.  *
 *                                                                         *
 *          flags          -- Memory allocation control flags.             *
 *             MEM_NORMAL: No special flags.                               *
 *             MEM_CLEAR:  Zero out memory block.                        	*
 *             MEM_NEW:		Called by a new.                                *
 *                                                                         *
 * OUTPUT:  Returns with pointer to allocated block.  If NULL was returned *
 *          it indicates a failure to allocate.  Note: NULL will never be  *
 *          returned if the standard library allocation error routine is   *
 *          used.                                                          *
 *                                                                         *
 * WARNINGS:   If you replace the standard memory allocation error routine *
 *             and make it so that Alloc CAN return with a NULL, be sure   *
 *             and check for this in your code.                            *
 *                                                                         *
 * HISTORY:                                                                *
 *   09/03/1991 JLB : Documented.                                          *
 *   08/09/1993 JLB : Updated with EMS memory support.                     *
 *   04/28/1994 JAW : Updated to 32bit Protected mode.                     *
 *   03/09/1995 JLB : Fixed                                                *
 *=========================================================================*/
void *Alloc(unsigned long bytes_to_alloc, MemoryFlagType flags)
{
	//	union REGS		regs ;
	//	struct SREGS 	sregs ;
	//	unsigned char *retval=NULL;	// Pointer to allocated block.
	//	unsigned long	original_size;	// Original allocation size.
	//	unsigned long	bytesfree;		// Number of free bytes.
	//	long 				*longptr=NULL;	// Pointer used to store selector
	//	static unsigned char _allocinit=0;
	//
	//
	//	//
	//	// Init memory system by finding largest block to alloc
	//	// then allocate it to get one large heap and free it.
	//	// There may be more memory available from DPMI but we only are
	//	// for now allocating and freeing the first largest block.
	//	//
	//	if ( !_allocinit ) {
	//		unsigned long largestblock = Largest_Mem_Block();
	//		largestblock -= 1024;				// subtract for heap header and misc
	//		largestblock &= 0xffff0000;		// forcing to 64K boundary
	//
	//		if ( largestblock ) {
	//			LargestRamBlock = MIN( largestblock, RequestedSystemRam );
	//			unsigned char *lptr = (unsigned char *)malloc( LargestRamBlock );
	//			if ( lptr ) {
	//				free( (void *)lptr );
	//			}
	//		}
	//
	//		/*
	//		**	Initialize the total ram available value.
	//		*/
	//		TotalRam = Total_Ram_Free(MEM_NORMAL);
	//
	//		_allocinit = 1;
	//	}
	//
	//	/*
	//	** Save the original allocated space size so that we can clear the
	//	** exact amount of RAM if they specified MEM_CLEAR.
	//	*/
	//	original_size = bytes_to_alloc;
	//
	//	/*
	//	** Reserve one byte for the header of the memory we allocated.
	//	** We will store the flags variable there for later use.
	//	*/
	// #if (LONG_ALIGNMENT)
	//	bytes_to_alloc += (flags & MEM_LOCK) ? 8 : 4;
	// #else
	//	bytes_to_alloc += (flags & MEM_LOCK) ? 5 : 1;
	// #endif
	//
	//
	//	// Try to allocate the memory out of the protected mode memory
	//	// chain if we did not require a real mode allocation.  If this
	//	// fails we will have to try to allocate it out of real mode memory.
	//	// Real mode memory is a last resort because some types of applications
	//	// require real mode memory.
	//	if (!(flags & MEM_REAL)) {
	//		retval = (unsigned char*)malloc(bytes_to_alloc);
	//	}
	//
	//	// Try to allocate the memory out of the real mode memory using DPMI
	//	// service 0x100.  Note that retval will be null if we are requesting
	//	// real mode memory so that we do not have to explicitly check for the
	//	// real mode flag.  Remember we need to reserve room for the dos
	//	// selector value at the beginning of our allocated block so rather than
	//	// adding fifteen and rounding, we need to add 19 and round.
	//	if (!retval) {
	//		flags = (MemoryFlagType)(flags | MEM_REAL);
	//		regs.x.eax = 0x100;
	//		regs.x.ebx = (bytes_to_alloc + 19) >> 4;
	//		if (regs.x.ebx & 0xFFFF0000) {
	//			retval = NULL;
	//		} else {
	//			segread ( & sregs ) ;
	//			int386x ( 0x31 , & regs, & regs , & sregs	) ;
	//			if (regs.x.cflag)
	//				retval = NULL;
	//			else {
	// #if (LONG_ALIGNMENT)
	//				longptr		= (long *)(((regs.x.eax & 0xFFFF) << 4)+ 4);
	// #else
	//				longptr		= (long *)(((regs.x.eax & 0xFFFF) << 4)+ 1);
	// #endif
	//				*longptr++	= regs.x.edx & 0xFFFF;
	//				retval		= (unsigned char *)longptr;
	//			}
	//		}
	//	}
	//
	//	// If the alloc failed then we need to signify a memory error.
	//	if (retval == NULL) {
	//		if (Memory_Error != NULL)
	//			Memory_Error();
	//		return NULL;
	//	}
	//
	//	// If the memory needs to be DPMI locked then we should store the
	//	// original size in the header before we store the flags.
	//	if (flags & MEM_LOCK) {
	//		longptr		= (long *)retval;
	//		*longptr++	= original_size;
	//		retval		= (unsigned char *)longptr;
	//	}
	//
	//
	//	// Now that we know the alloc was sucessful (and for an extra byte
	//	// more than the user wanted) we need to stick in the memory flags.
	// #if (LONG_ALIGNMENT)
	//	if ( !(flags & (MEM_LOCK|MEM_REAL)) ) {
	//		//
	//		// WARNING!!!!!!!!!!
	//		// USE this only with the WATCOM malloc ALLOCATION!!!!!!!!!
	//		// it reads the actual block size before the ptr returned.
	//		// then eors and uses the upper word for a validation later on free.
	//		//
	//		longptr = (long *)retval;
	//		*longptr = ((*(longptr - 1)) ^ 0xffffffff) & 0xffff0000;
	//		*retval++ 		= flags;
	//		*retval++ 		= (unsigned char)(flags ^ 0xff);
	//		retval += 2;
	//	}
	//	else {
	//		*retval++ 		= flags;
	//		*retval++ 		= (unsigned char)(flags ^ 0xff);
	//		*retval++ 		= 0;
	//		*retval++ 		= 0;
	//	}
	// #else
	//	*retval++ 		= (unsigned char)(flags | (((flags ^ 0x07) & 0x07) << 5));
	// #endif
	//
	//	// If the memory needed to be DPMI locked then set it up so it
	//	// is locked.
	//	if (flags & MEM_LOCK) {
	//		DPMI_Lock(retval, original_size);
	//	}
	//
	//	/* Clear the space if they wanted it clear */
	//
	//	if (flags & MEM_CLEAR) {
	//		unsigned char	*ptr;		// Working memory block pointer.
	//
	//		ptr = retval;
	//		memset(ptr, '\0', original_size);
	//	}
	//
	//	bytesfree = Total_Ram_Free(MEM_NORMAL);
	//	if (bytesfree < MinRam) {
	//		MinRam = bytesfree;
	//	}
	//	if (TotalRam-bytesfree > MaxRam) {
	//		MaxRam = TotalRam-bytesfree;
	//	}
	//
	//	Memory_Calls++;
	//
	// #if(LOGGING)
	//	int val = _heapchk();
	//
	//	FILE *file = fopen("mem.txt","at");
	//	fprintf(file, "%P Alloc size = %d, Actual Size = %d, flags = %d, heap = %d\n",
	//						retval,
	//						original_size,
	//						bytes_to_alloc,
	//						flags,
	//						val);
	//	fclose(file);
	// #endif
	//
	//	return(retval);
	size_t size = bytes_to_alloc;
	const size_t headerSize = sizeof(BlockMeta);
	const size_t totalSize = headerSize + size;

	// 内存分配
	void *basePtr = nullptr;
#ifdef WIN32
	basePtr = VirtualAlloc(nullptr, totalSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
	basePtr = aligned_alloc(sysconf(_SC_PAGESIZE), totalSize);
#endif

	if (!basePtr)
		throw std::bad_alloc();

	// 初始化元数据
	BlockMeta *meta = static_cast<BlockMeta *>(basePtr);
	meta->size = size;
	meta->flags = flags;
	meta->checksum = static_cast<uint32_t>(size ^ flags);

	// 内存锁定处理
	if (flags & MEM_LOCK)
	{
		try
		{
			MemorySystem::LockMemory(basePtr, totalSize);
		}
		catch (...)
		{
#ifdef WIN32
			VirtualFree(basePtr, 0, MEM_RELEASE);
#else
			free(basePtr);
#endif
			throw;
		}
	}

	// 内存清零
	void *userPtr = reinterpret_cast<uint8_t *>(basePtr) + headerSize;
	if (flags & MEM_CLEAR)
	{
		memset(userPtr, 0, size);
	}

	// 更新追踪数据
	MemorySystem::totalAllocated += totalSize;
	MemorySystem::peakAllocated = max(
		MemorySystem::peakAllocated.load(),
		MemorySystem::totalAllocated.load());
	MemorySystem::allocationCount++;

	return userPtr;
}

/***************************************************************************
 * Free -- Free an Alloc'ed block of RAM.                                  *
 *                                                                         *
 * FUNCTION:                                                               *
 *                                                                         *
 * INPUT:       A pointer to a block of RAM from Alloc.                    *
 *                                                                         *
 * OUTPUT:      None.                                                      *
 *                                                                         *
 * WARNINGS:    Don't use this for an Alloc_Block'ed RAM block.            *
 *                                                                         *
 * HISTORY:                                                                *
 *   05/25/1990     : Created.                                             *
 ***************************************************************************/
void Free(void const *pointer) noexcept
{
//	union REGS regs;
//	struct SREGS sregs;
//
//	//	void const *original = pointer;
//	char string[80];
//
//	if (pointer)
//	{
//		/*
//		** Get a pointer to the flags that we stored off.
//		*/
//#if (LONG_ALIGNMENT)
//		unsigned char *byteptr = ((unsigned char *)pointer) - 4;
//
//		//
//		// validate the flags with and eor of the flags
//		//
//		if (*byteptr != ((*(byteptr + 1)) ^ 0xff))
//		{
//			if (Memory_Error_Exit != NULL)
//			{
//				sprintf(string, "Error freeing pointer %p.  Header invalid!!!\n", pointer);
//				Memory_Error_Exit(string);
//			}
//		}
//		else
//		{
//			if (!(*byteptr & (MEM_LOCK | MEM_REAL)))
//			{
//				unsigned short *wordptr = (unsigned short *)(byteptr - 2);
//
//				//
//				// WARNING!!!!!!!!!!
//				// USE this only with the WATCOM malloc ALLOCATION!!!!!!!!!
//				// it reads the actual block size before the ptr to be freed.
//				// then compares with the EOR to the value stored during allocation.
//				//
//				if (*wordptr != ((*(wordptr + 2)) ^ 0xffff))
//				{
//					if (Memory_Error_Exit != NULL)
//					{
//						sprintf(string, "Error freeing pointer %p.  Header invalid!!!\n", pointer);
//						Memory_Error_Exit(string);
//					}
//				}
//			}
//			else if (*(byteptr + 2) || *(byteptr + 3))
//			{
//				if (Memory_Error_Exit != NULL)
//				{
//					sprintf(string, "Error freeing pointer %p.  Header invalid!!!\n", pointer);
//					Memory_Error_Exit(string);
//				}
//			}
//		}
////		if ( *byteptr != (*(byteptr + 1) ^ 0xff) ||
////			*(byteptr + 2) || *(byteptr + 3) ) {
////			if (Memory_Error_Exit != NULL) {
////				sprintf( string, "Error freeing pointer %p.  Header invalid!!!\n", pointer );
////				Memory_Error_Exit( string );
////			}
////		}
//#else
//		unsigned char *byteptr = ((unsigned char *)pointer) - 1;
//
//		if ((*byteptr & 0xe0) != (((*byteptr ^ 0x07) & 0x07) << 5))
//		{
//			if (Memory_Error_Exit != NULL)
//			{
//				sprintf(string, "Error freeing pointer %p.  Header invalid!!!\n", pointer);
//				Memory_Error_Exit(string);
//			}
//		}
//#endif
//
//		/*
//		** Check to see if this was locked me and if it was unlock it.
//		*/
//		if (*byteptr & MEM_LOCK)
//		{
//			long *longptr = ((long *)byteptr) - 1;
//			DPMI_Unlock(pointer, *longptr);
//			pointer = (void *)longptr;
//		}
//		else
//			pointer = (void *)byteptr;
//
//#if (LOGGING)
//		int val = _heapchk();
//		FILE *file = fopen("mem.txt", "at");
//		fprintf(file, "%P Free flags = %d, Heap = %d\n",
//				original,
//				*byteptr,
//				val);
//		fclose(file);
//#endif
//
//		// If the pointer is a real mode pointer than it will point to the
//		// first megabyte of system memory.  If it does than we need to
//		// use DPMI to free it.
//		if (*byteptr & MEM_REAL)
//		{
//			regs.x.eax = 0x101;
//			regs.x.edx = *(((long *)pointer) - 1);
//			segread(&sregs);
//			int386x(0x31, &regs, &regs, &sregs);
//		}
//		else
//		{
//			free((void *)pointer);
//		}
//		Memory_Calls--;
//	}
	void* userPtr = const_cast<void*>(pointer);
	if (!userPtr) return;

	BlockMeta* meta = reinterpret_cast<BlockMeta*>(
		reinterpret_cast<uint8_t*>(userPtr) - sizeof(BlockMeta)
		);

	try {
		MemorySystem::ValidateBlock(meta);
	}
	catch (...) {
		// 记录错误但继续执行
		std::cerr << "Memory validation failed during free" << std::endl;
	}

	const size_t totalSize = sizeof(BlockMeta) + meta->size;

	// 内存解锁
	if (meta->flags & MEM_LOCK) {
		MemorySystem::UnlockMemory(meta, totalSize);
	}

	// 内存释放
	#ifdef WIN32
	VirtualFree(meta, 0, MEM_RELEASE);
	#else
	free(meta);
	#endif

	// 更新追踪数据
	MemorySystem::totalAllocated -= totalSize;
	MemorySystem::allocationCount--;

}

/***************************************************************************
 * Resize_Alloc -- Change the size of an allocated block.                  *
 *                                                                         *
 *    This routine will take a previously allocated block and change its   *
 *    size without unnecessarily altering its contents.                    *
 *                                                                         *
 * INPUT:   pointer  -- Pointer to the original memory allocation.         *
 *                                                                         *
 *          new_size -- Size in bytes that it will be converted to.        *
 *                                                                         *
 * OUTPUT:  Returns with a pointer to the new allocation.                  *
 *                                                                         *
 * WARNINGS:   ???                                                         *
 *                                                                         *
 * HISTORY:                                                                *
 *   02/01/1992 JLB : Commented.                                           *
 *=========================================================================*/
void *Resize_Alloc(void *original_ptr, unsigned long new_size_in_bytes)
{

	//unsigned long *temp;
	////	unsigned long diff, flags;

	//temp = (unsigned long *)original_ptr;

	///* ReAlloc the space */
	//temp = (unsigned long *)realloc(temp, new_size_in_bytes);
	//if (temp == NULL)
	//{
	//	if (Memory_Error != NULL)
	//		Memory_Error();
	//	return NULL;
	//}

	//return (temp);
	void* oldPtr = original_ptr;
	size_t newSize = new_size_in_bytes;
	if (!oldPtr) return ModernAlloc(newSize);

	BlockMeta* oldMeta = reinterpret_cast<BlockMeta*>(
		reinterpret_cast<uint8_t*>(oldPtr) - sizeof(BlockMeta)
		);
	MemorySystem::ValidateBlock(oldMeta);

	// 分配新内存
	void* newPtr = ModernAlloc(newSize, oldMeta->flags);

	// 复制数据
	const size_t copySize = min(oldMeta->size, newSize);
	memcpy(newPtr, oldPtr, copySize);

	// 释放旧内存
	Free(oldPtr);

	return newPtr;
}

/***************************************************************************
 * Ram_Free -- Determines the largest free chunk of RAM.                   *
 *                                                                         *
 *    Use this routine to determine the largest free chunk of available    *
 *    RAM for allocation.  It also performs a check of the memory chain.   *
 *                                                                         *
 * INPUT:   none                                                           *
 *                                                                         *
 * OUTPUT:  Returns with the size of the largest free chunk of RAM.        *
 *                                                                         *
 * WARNINGS:   This does not return the TOTAL memory free, only the        *
 *             largest free chunk.                                         *
 *                                                                         *
 * HISTORY:                                                                *
 *   09/03/1991 JLB : Commented.                                           *
 *=========================================================================*/
long Ram_Free(MemoryFlagType)
{
	//return (_memmax());
	//	return Largest_Mem_Block();

#ifdef WIN32
	MEMORYSTATUSEX status = { sizeof(status) };
	GlobalMemoryStatusEx(&status);
	return status.ullAvailPhys;
#else
	long pageSize = sysconf(_SC_PAGESIZE);
	long availPages = sysconf(_SC_AVPHYS_PAGES);
	return pageSize * availPages;
#endif
}

/***************************************************************************
 * Heap_Size -- Size of the heap we have.                                  *
 *                                                                         *
 *                                                                         *
 *                                                                         *
 * INPUT:                                                                  *
 *                                                                         *
 * OUTPUT:                                                                 *
 *                                                                         *
 * WARNINGS:                                                               *
 *                                                                         *
 * HISTORY:                                                                *
 *   06/21/1994 SKB : Created.                                             *
 *=========================================================================*/
long Heap_Size(MemoryFlagType) noexcept
{
	//if (!TotalRam)
	//{
	//	TotalRam = Total_Ram_Free(MEM_NORMAL);
	//}
	//return (TotalRam);

	return MemorySystem::totalAllocated.load();
}

/***************************************************************************
 * Total_Ram_Free -- Total amount of free RAM.                             *
 *                                                                         *
 *                                                                         *
 *                                                                         *
 * INPUT:                                                                  *
 *                                                                         *
 * OUTPUT:                                                                 *
 *                                                                         *
 * WARNINGS:                                                               *
 *                                                                         *
 * HISTORY:                                                                *
 *   06/21/1994 SKB : Created.                                             *
 *   03/09/1995 JLB : Uses prerecorded heap size maximum.                  *
 *=========================================================================*/
long Total_Ram_Free(MemoryFlagType) noexcept
{
	//return (_memavl());
	//	//return Largest_Mem_Block () ;
#ifdef WIN32
// Windows 实现
	MEMORYSTATUSEX status = { sizeof(status) };
	if (!GlobalMemoryStatusEx(&status)) {
		return 0; // 可通过 GetLastError() 获取详细信息
	}
	return status.ullAvailPhys;

#else
	// Linux/Unix 实现方案一：使用 sysinfo
	struct sysinfo info;
	if (sysinfo(&info) == 0) {
		return info.freeram * info.mem_unit;
	}

	// 方案二：解析 /proc/meminfo（更精确）
	try {
		std::ifstream meminfo("/proc/meminfo");
		std::string line;
		size_t available = 0;

		while (std::getline(meminfo, line)) {
			if (line.find("MemAvailable:") != std::string::npos) {
				std::istringstream iss(line);
				std::string label;
				size_t kb;
				iss >> label >> kb;
				available = kb * 1024; // 转换为字节
				break;
			}
		}
		return available ? available : 0;
	}
	catch (...) {
		return 0;
	}
#endif

}

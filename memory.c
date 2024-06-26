#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
#include <elf.h>
#include <assert.h>
#include <pthread.h>
#include "memory.h"

typedef unsigned long long ulong64;
#define MAGIC_ADDR 0x12abcdef
#define PATH_SZ 128

#define SEGMENT_SIZE (4ULL << 30)
#define PAGE_SIZE 4096
#define METADATA_SIZE ((SEGMENT_SIZE / PAGE_SIZE) * 2)
#define NUM_PAGES_IN_SEG (METADATA_SIZE / 2)
#define OTHER_METADATA_SIZE ((METADATA_SIZE / PAGE_SIZE) * 2)
#define COMMIT_SIZE PAGE_SIZE
#define Align(x, y) (((x) + (y - 1)) & ~(y - 1))
#define ADDR_TO_PAGE(x) (char *)(((ulong64)(x)) & ~(PAGE_SIZE - 1))
#define ADDR_TO_SEGMENT(x) (Segment *)(((ulong64)(x)) & ~(SEGMENT_SIZE - 1))
#define FREE 1
#define MARK 2
#define GC_THRESHOLD (32ULL << 20)

long long NumGCTriggered = 0;
long long NumBytesFreed = 0;
long long NumBytesAllocated = 0;
extern char etext, edata, end;
int unscannedListCount = 0;

struct OtherMetadata
{
	char *AllocPtr;
	char *CommitPtr;
	char *ReservePtr;
	char *DataPtr;
	int BigAlloc;
};

typedef struct Segment
{
	union
	{
		unsigned short Size[NUM_PAGES_IN_SEG];
		struct OtherMetadata Other;
	};
} Segment;

typedef struct SegmentList
{
	struct Segment *Segment;
	struct SegmentList *Next;
} SegmentList;

typedef struct ObjHeader
{
	unsigned Size;
	unsigned Status;
	ulong64 Type;
} ObjHeader;

// Unscanned List Node represents a node in the Unscanned List.
// It contains a pointer to the object header and a pointer to the next node in the list.
typedef struct UnscannedListNode
{
	struct ObjHeader *Object;
	struct UnscannedListNode *Next;
} UnscannedListNode;

// The Unscanned List is a linked list of Unscanned List Nodes.
// It contains a pointer to the head and tail of the list.
typedef struct UnscannedList
{
	struct UnscannedListNode *Head;
	struct UnscannedListNode *Tail;
} UnscannedList;

#define OBJ_HEADER_SIZE (sizeof(ObjHeader))

static SegmentList *Segments = NULL;
static UnscannedList *Unscanned = NULL;

static void setAllocPtr(Segment *Seg, char *Ptr) { Seg->Other.AllocPtr = Ptr; }
static void setCommitPtr(Segment *Seg, char *Ptr) { Seg->Other.CommitPtr = Ptr; }
static void setReservePtr(Segment *Seg, char *Ptr) { Seg->Other.ReservePtr = Ptr; }
static void setDataPtr(Segment *Seg, char *Ptr) { Seg->Other.DataPtr = Ptr; }
static char *getAllocPtr(Segment *Seg) { return Seg->Other.AllocPtr; }
static char *getCommitPtr(Segment *Seg) { return Seg->Other.CommitPtr; }
static char *getReservePtr(Segment *Seg) { return Seg->Other.ReservePtr; }
static char *getDataPtr(Segment *Seg) { return Seg->Other.DataPtr; }
static void setBigAlloc(Segment *Seg, int BigAlloc) { Seg->Other.BigAlloc = BigAlloc; }
static int getBigAlloc(Segment *Seg) { return Seg->Other.BigAlloc; }
static void myfree(void *Ptr);
static void checkAndRunGC();

static void addToSegmentList(Segment *Seg)
{
	SegmentList *L = malloc(sizeof(SegmentList));
	if (L == NULL)
	{
		printf("Unable to allocate list node\n");
		exit(0);
	}
	L->Segment = Seg;
	L->Next = Segments;
	Segments = L;
}

static void addToUnscannedList(struct ObjHeader *Object)
{
	struct UnscannedListNode *Node = malloc(sizeof(struct UnscannedListNode));
	if (Node == NULL)
	{
		printf("Unable to allocate list node\n");
		exit(0);
	}
	Node->Object = Object;
	Node->Next = NULL;
	if (Unscanned == NULL)
	{
		Unscanned = malloc(sizeof(struct UnscannedList));
		Unscanned->Head = Node;
		Unscanned->Tail = Node;
	}
	else if (Unscanned->Head == NULL || Unscanned->Tail == NULL)
	{
		Unscanned->Head = Node;
		Unscanned->Tail = Node;
	}
	else
	{
		Unscanned->Tail->Next = Node;
		Unscanned->Tail = Node;
	}
}

static void allowAccess(void *Ptr, size_t Size)
{
	assert((Size % PAGE_SIZE) == 0);
	assert(((ulong64)Ptr & (PAGE_SIZE - 1)) == 0);

	int Ret = mprotect(Ptr, Size, PROT_READ | PROT_WRITE);
	if (Ret == -1)
	{
		printf("unable to mprotect %s():%d\n", __func__, __LINE__);
		exit(0);
	}
}

static Segment *allocateSegment(int BigAlloc)
{
	void *Base = mmap(NULL, SEGMENT_SIZE * 2, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (Base == MAP_FAILED)
	{
		printf("unable to allocate a segment\n");
		exit(0);
	}

	/* segments are aligned to segment size */
	Segment *Segment = (struct Segment *)Align((ulong64)Base, SEGMENT_SIZE);
	allowAccess(Segment, METADATA_SIZE);

	char *AllocPtr = (char *)Segment + METADATA_SIZE;
	char *ReservePtr = (char *)Segment + SEGMENT_SIZE;
	setAllocPtr(Segment, AllocPtr);
	setReservePtr(Segment, ReservePtr);
	setCommitPtr(Segment, AllocPtr);
	setDataPtr(Segment, AllocPtr);
	setBigAlloc(Segment, BigAlloc);
	addToSegmentList(Segment);
	return Segment;
}

static void extendCommitSpace(Segment *Seg)
{
	char *AllocPtr = getAllocPtr(Seg);
	char *CommitPtr = getCommitPtr(Seg);
	char *ReservePtr = getReservePtr(Seg);
	char *NewCommitPtr = CommitPtr + COMMIT_SIZE;

	assert(AllocPtr == CommitPtr);
	if (NewCommitPtr <= ReservePtr)
	{
		allowAccess(CommitPtr, COMMIT_SIZE);
		setCommitPtr(Seg, NewCommitPtr);
	}
	else
	{
		assert(CommitPtr == ReservePtr);
	}
}

static unsigned short *getSizeMetadata(char *Ptr)
{
	char *Page = ADDR_TO_PAGE(Ptr);
	Segment *Seg = ADDR_TO_SEGMENT(Ptr);
	ulong64 PageNo = (Page - (char *)Seg) / PAGE_SIZE;
	return &Seg->Size[PageNo];
}

static void createHole(Segment *Seg)
{
	char *AllocPtr = getAllocPtr(Seg);
	char *CommitPtr = getCommitPtr(Seg);
	size_t HoleSz = CommitPtr - AllocPtr;
	if (HoleSz > 0)
	{
		assert(HoleSz >= 8);
		ObjHeader *Header = (ObjHeader *)AllocPtr;
		Header->Size = HoleSz;
		Header->Status = 0;
		setAllocPtr(Seg, CommitPtr);
		myfree(AllocPtr + OBJ_HEADER_SIZE);
		NumBytesFreed -= HoleSz;
	}
}

static void reclaimMemory(void *Ptr, size_t Size)
{
	assert((Size % PAGE_SIZE) == 0);
	assert(((ulong64)Ptr & (PAGE_SIZE - 1)) == 0);

	int Ret = mprotect(Ptr, Size, PROT_NONE);
	if (Ret == -1)
	{
		printf("unable to mprotect %s():%d\n", __func__, __LINE__);
		exit(0);
	}
	Ret = madvise(Ptr, Size, MADV_DONTNEED);
	if (Ret == -1)
	{
		printf("unable to reclaim physical page %s():%d\n", __func__, __LINE__);
		exit(0);
	}
}

/* used by the GC to free objects. */
static void myfree(void *Ptr)
{
	ObjHeader *Header = (ObjHeader *)((char *)Ptr - OBJ_HEADER_SIZE);
	assert((Header->Status & FREE) == 0);
	NumBytesFreed += Header->Size;
	if (Header->Size > COMMIT_SIZE)
	{
		assert((Header->Size % PAGE_SIZE) == 0);
		assert(((ulong64)Header & (PAGE_SIZE - 1)) == 0);
		size_t Size = Header->Size;
		char *Start = (char *)Header;
		size_t Iter;
		for (Iter = 0; Iter < Size; Iter += PAGE_SIZE)
		{
			unsigned short *SzMeta = getSizeMetadata((char *)Start + Iter);
			SzMeta[0] = PAGE_SIZE;
		}
		Header->Status = FREE;
		reclaimMemory(Header, Header->Size);
		return;
	}

	unsigned short *SzMeta = getSizeMetadata((char *)Header);
	SzMeta[0] += Header->Size;
	assert(SzMeta[0] <= PAGE_SIZE);
	Header->Status = FREE;
	if (SzMeta[0] == PAGE_SIZE)
	{
		char *Page = ADDR_TO_PAGE(Ptr);
		reclaimMemory(Page, PAGE_SIZE);
	}
}

static void *BigAlloc(size_t Size)
{
	size_t AlignedSize = Align(Size + OBJ_HEADER_SIZE, PAGE_SIZE);
	assert(AlignedSize <= SEGMENT_SIZE - METADATA_SIZE);
	static Segment *CurSeg = NULL;
	if (CurSeg == NULL)
	{
		CurSeg = allocateSegment(1);
	}
	char *AllocPtr = getAllocPtr(CurSeg);
	char *CommitPtr = getCommitPtr(CurSeg);
	char *NewAllocPtr = AllocPtr + AlignedSize;
	char *ReservePtr = getReservePtr(CurSeg);
	if (NewAllocPtr > ReservePtr)
	{
		CurSeg = allocateSegment(1);
		return BigAlloc(Size);
	}
	NumBytesAllocated += AlignedSize;
	assert(AllocPtr == CommitPtr);
	allowAccess(CommitPtr, AlignedSize);
	setAllocPtr(CurSeg, NewAllocPtr);
	setCommitPtr(CurSeg, NewAllocPtr);

	unsigned short *SzMeta = getSizeMetadata(AllocPtr);
	SzMeta[0] = 1;

	ObjHeader *Header = (ObjHeader *)AllocPtr;
	Header->Size = AlignedSize;
	Header->Status = 0;
	Header->Type = 0;
	return AllocPtr + OBJ_HEADER_SIZE;
}

void *_mymalloc(size_t Size)
{
	size_t AlignedSize = Align(Size, 8) + OBJ_HEADER_SIZE;

	checkAndRunGC(AlignedSize);
	if (AlignedSize > COMMIT_SIZE)
	{
		return BigAlloc(Size);
	}
	assert(Size != 0);
	assert(sizeof(struct OtherMetadata) <= OTHER_METADATA_SIZE);
	assert(sizeof(struct Segment) == METADATA_SIZE);

	static Segment *CurSeg = NULL;

	if (CurSeg == NULL)
	{
		CurSeg = allocateSegment(0);
	}
	char *AllocPtr = getAllocPtr(CurSeg);
	char *CommitPtr = getCommitPtr(CurSeg);
	char *NewAllocPtr = AllocPtr + AlignedSize;
	if (NewAllocPtr > CommitPtr)
	{
		if (AllocPtr != CommitPtr)
		{
			/* Free remaining space on this page */
			createHole(CurSeg);
		}
		extendCommitSpace(CurSeg);
		AllocPtr = getAllocPtr(CurSeg);
		NewAllocPtr = AllocPtr + AlignedSize;
		CommitPtr = getCommitPtr(CurSeg);
		if (NewAllocPtr > CommitPtr)
		{
			CurSeg = allocateSegment(0);
			return _mymalloc(Size);
		}
	}

	NumBytesAllocated += AlignedSize;
	setAllocPtr(CurSeg, NewAllocPtr);
	ObjHeader *Header = (ObjHeader *)AllocPtr;
	Header->Size = AlignedSize;
	Header->Status = 0;
	Header->Type = 0;
	return AllocPtr + OBJ_HEADER_SIZE;
}

// retrieveObjectHeader is a helper function that retrieves the object header for the 8-byte object at the address.
// The function takes w (the 8-byte value), the segment in which the object lies and a flag to check if the object is a big allocation.
static char *retrieveObjectHeader(int isBigAlloc, char *W, Segment *foundSegment)
{
	// Get the metadata for the page to which the object belongs.
	unsigned short *sizeMetadata = getSizeMetadata(W);

	// Check if the page has been allocated or not.
	// Check if the object is freed or not.
	if (sizeMetadata[0] == PAGE_SIZE)
	{
		// The page is free.
		// This means that the object is not a valid object.
		return NULL;
	}

	// Taken from getSizeMetadata, refer to the function for more details.
	char *pageForObject = ADDR_TO_PAGE(W);
	ulong64 pageNoForObject = (pageForObject - (char *)foundSegment) / PAGE_SIZE;

	// Check if the allocation is a big allocation.
	if (isBigAlloc == 0)
	{
		// The object is not a big allocation.
		// Iterate through all the objects in this page.
		// Check if the object is the object we are looking for.
		char *currentObject = pageForObject;
		ObjHeader *currentObjectHeader = (ObjHeader *)pageForObject;
		// We check two conditions:
		// 1. The value lies between the address of the object and the address of the object + size of the object.
		// 2. The pointer to the object is less than the address of the page + PAGE_SIZE.
		// When these two conditions are met, this means that the object is the object we are looking for.
		char *endAddress = currentObject + currentObjectHeader->Size;
		char *startAddress = currentObject + OBJ_HEADER_SIZE;
		char *pageBounds = pageForObject + PAGE_SIZE;
		int withinPageBounds = 0;
		int withinObjectBounds = 0;

		if (W <= endAddress && W >= startAddress)
		{
			withinObjectBounds = 1;
		}

		if (W <= pageBounds)
		{
			withinPageBounds = 1;
		}

		while (!(withinObjectBounds == 1) && (withinPageBounds == 1))
		{
			currentObject = currentObject + currentObjectHeader->Size;
			currentObjectHeader = (ObjHeader *)currentObject;
			endAddress = currentObject + currentObjectHeader->Size;
			startAddress = currentObject + OBJ_HEADER_SIZE;
			pageBounds = pageForObject + PAGE_SIZE;

			if (W <= endAddress && W >= startAddress)
			{
				withinObjectBounds = 1;
			}
			else
			{
				withinObjectBounds = 0;
			}

			if (W <= pageBounds)
			{
				withinPageBounds = 1;
			}
			else
			{
				withinPageBounds = 0;
			}
		}
		return currentObject;
	}
	else
	{
		// The object is a big allocation.
		// We traverse backwards and try to find the first page for this big allocation.
		for (; foundSegment->Size[pageNoForObject] != 1; pageNoForObject--, pageForObject -= PAGE_SIZE)
			;
		return pageForObject;
	}
}

// markValidObject checks if the 8-byte object at the address belongs to a heap object.
// For this, we iterate through the segments and check if the address lies between the data
// pointer and the alloc pointer of the segment.
// If it does, we retrive the object header using retrieveObjectHeader and mark the object for scanning.
static void markValidObject(char *pointer)
{
	// Keeping track of the iterator for the current segment.
	// This is used to iterate through all the segments.
	SegmentList *L = Segments;

	// The segment in which the pointer lies.
	Segment *foundSegment = NULL;

	// Extracting the 8-byte value at the address.
	// Deference the pointer to get the 8-byte value stores at the memory location.
	// This reinterprets the 8 bytes of the integer as a sequence of 8 characters.
	// Refer Lect-15 slides.
	char *W = (char *)(*((ulong64 *)pointer));

	// Iterating over all the segments to check if the 8-byte values stored at the pointer address
	// lies between the data pointer and the alloc pointer of the segment.
	while (L != NULL)
	{
		Segment *curSeg = L->Segment;
		char *dataPtr = getDataPtr(curSeg);
		char *allocPtr = getAllocPtr(curSeg);

		if (dataPtr <= W && W <= allocPtr)
		{
			foundSegment = curSeg;

			break;
		}
		L = L->Next;
	}

	if (foundSegment == NULL)
	{
		// Not a valid object.
		// Does not belong to the heap.
		return;
	}

	// Marking the object for scanning.
	int isBigAlloc = getBigAlloc(foundSegment);
	char *objectHeader = retrieveObjectHeader(isBigAlloc, W, foundSegment);

	if (objectHeader == NULL)
	{
		// No object header was found.
		// This means that the object is not a valid object.
		return;
	}

	// Check if we are supposed to mark the object and add it to the unscanned list.
	ObjHeader *object = (ObjHeader *)objectHeader;
	if (object->Status == 0)
	{
		object->Status = MARK;
		addToUnscannedList(object);
		unscannedListCount++;
	}
}

/* scan objects in the scanner list.
 * add newly encountered unmarked objects
 * to the scanner list after marking them.
 */
void scanner()
{
	// Traverse Unscanned List.
	UnscannedListNode *currentNode = Unscanned->Head;
	int count = 0;
	// Print the number of objects in the unscanned list.
	// Traverse scanner list and count.
	UnscannedListNode *temp = Unscanned->Head;
	while (temp != NULL)
	{
		count++;
		temp = temp->Next;
	}
	printf("Number of objects in the unscanned list: %d\n", count);
	while (currentNode != NULL)
	{
		ObjHeader *currentObject = currentNode->Object;
		char *objectStart = (char *)currentObject + OBJ_HEADER_SIZE;
		char *objectEnd = (char *)currentObject + currentObject->Size;
		for (char *pointer = objectStart; pointer <= objectEnd - 8; pointer++)
		{
			markValidObject(pointer);
		}
		// Remove currentNode from the list and move to the next node for traversal.
		Unscanned->Head = Unscanned->Head->Next;
		currentNode = currentNode->Next;
	}

	if (Unscanned->Head == NULL)
	{
		Unscanned->Head = NULL;
		Unscanned->Tail = NULL;
	}

	count = 0;
	// Print the number of objects in the unscanned list.
	// Traverse scanner list and count.
	temp = Unscanned->Head;
	while (temp != NULL)
	{
		count++;
		temp = temp->Next;
	}
	printf("Number of objects in the unscanned list: %d\n", count);
}

static ObjHeader *markOrFreeObject(ObjHeader *currentObjectHeader)
{
	ObjHeader *objectToBeFreed = NULL;

	// Set the status of the marked object to 0.
	if (currentObjectHeader->Status == MARK)
	{
		currentObjectHeader->Status = 0;
	}
	// Free the objects that have not been marked.
	else if (currentObjectHeader->Status == 0)
	{
		objectToBeFreed = currentObjectHeader;
	}
	return objectToBeFreed;
}

static void sweepBigAllocation(Segment *curSeg, char *currentPage)
{
	char *allocPtr = getAllocPtr(curSeg);
	for (; currentPage < allocPtr; currentPage += PAGE_SIZE)
	{
		unsigned short *sizeMetadata = getSizeMetadata(currentPage);

		// Check if the page is free.
		int pageIsFree = (sizeMetadata[0] == PAGE_SIZE);
		if (pageIsFree)
		{
			// If the page is free then we move to the next page.
			continue;
		}

		int headerForBigAlloc = (sizeMetadata[0] == 1);
		char *currentObject = currentPage;
		ObjHeader *currentObjectHeader = (ObjHeader *)currentObject;
		unsigned sizeToBeFreed = currentObjectHeader->Size;

		// Need to check if this page hold the object header for the big allocation.
		if (headerForBigAlloc)
		{

			ObjHeader *objectToBeFreed = markOrFreeObject(currentObjectHeader);
			if (objectToBeFreed != NULL)
			{
				char *addressToPass = (char *)objectToBeFreed + OBJ_HEADER_SIZE;
				myfree(addressToPass);
			}

			currentPage += sizeToBeFreed;
		}
	}
}

static void traversePageForNormalAllocation(char *currentPage, unsigned short *sizeMetadata, Segment *curSeg)
{
	// For this page, we traverse all the objects.
	char *currentObject = currentPage;

	ObjHeader *objectToFree = NULL;

	// Need to ensure that we are within the boundaries of the page.
	// Need to ensure that we only free those objects that are unallocated.
	char *pageBoundary = currentPage + PAGE_SIZE;
	int withinBoundaries = (currentObject < pageBoundary && currentObject < getAllocPtr(curSeg));

	// Further, we need to ensure that while freeing the objects of a page, we do not try to call
	// free when the entire page has already been freed.
	while (sizeMetadata[0] != PAGE_SIZE && withinBoundaries)
	{
		ObjHeader *currentObjectHeader = (ObjHeader *)currentObject;

		ObjHeader *objectToFree = markOrFreeObject(currentObjectHeader);

		currentObject = currentObject + currentObjectHeader->Size;
		currentObjectHeader = (ObjHeader *)currentObject;

		// Free the object if we have found an object to be freed.
		if (objectToFree != NULL)
		{
			char *addressToPass = (char *)objectToFree + OBJ_HEADER_SIZE;
			myfree(addressToPass);
		}

		withinBoundaries = (currentObject < pageBoundary && currentObject < getAllocPtr(curSeg));
	}
}

static void sweepNormalAllocation(Segment *curSeg, char *currentPage)
{
	char *allocPtr = getAllocPtr(curSeg);
	// Traverse all the pages until the Alloc pointer.
	for (; currentPage < allocPtr; currentPage += PAGE_SIZE)
	{
		unsigned short *sizeMetadata = getSizeMetadata(currentPage);

		// Check if the page is free.
		int pageIsFree = (sizeMetadata[0] == PAGE_SIZE);
		if (pageIsFree)
		{
			// If the page is free then we move to the next page.
			continue;
		}

		traversePageForNormalAllocation(currentPage, sizeMetadata, curSeg);
	}
}

/* Free all unmarked objects. */
static void sweep()
{
	// Keeping track of the iterator for the current segment.
	// This is used to iterate through all the segments.
	SegmentList *L = Segments;

	// Iterating over all segments.
	for (; L != NULL; L = L->Next)
	{
		Segment *curSeg = L->Segment;

		// We iterate through all the pages of the segment.
		// We start from the data pointer and iterate through all the pages.
		char *currentPage = getDataPtr(curSeg);

		// Check if it is a big allocation or not.
		int isBigAlloc = getBigAlloc(curSeg);

		if (isBigAlloc == 0)
		{
			sweepNormalAllocation(curSeg, currentPage);
		}

		else if (isBigAlloc == 1)
		{
			sweepBigAllocation(curSeg, currentPage);
		}
	}
}

/* walk all addresses in the range [Top, Bottom-8].
 * add unmarked valid objects to the
 * scanner list after marking them
 * for scanning.
 */
static void scanRoots(unsigned char *Top, unsigned char *Bottom)
{
	char *pointer;
	unscannedListCount = 0;
	// Walking all the addresses in the range [Top, Bottom-8].
	// From Lecture - 15:
	// E.g., if the stack is in the range [x, y] walk all addresses in set S = {x, x+1, x+2,
	// ..., y-8}
	for (pointer = Top; (char *)pointer <= (char *)Bottom - 8; (char *)pointer++)
	{
		markValidObject(pointer);
	}
}

static size_t
getDataSecSz()
{
	char Exec[PATH_SZ];
	static size_t DsecSz = 0;

	if (DsecSz != 0)
	{
		return DsecSz;
	}
	DsecSz = -1;

	ssize_t Count = readlink("/proc/self/exe", Exec, PATH_SZ);

	if (Count == -1)
	{
		return -1;
	}
	Exec[Count] = '\0';

	int fd = open(Exec, O_RDONLY);
	if (fd == -1)
	{
		return -1;
	}

	struct stat Statbuf;
	fstat(fd, &Statbuf);

	char *Base = mmap(NULL, Statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (Base == NULL)
	{
		close(fd);
		return -1;
	}

	Elf64_Ehdr *Header = (Elf64_Ehdr *)Base;

	if (Header->e_ident[0] != 0x7f || Header->e_ident[1] != 'E' || Header->e_ident[2] != 'L' || Header->e_ident[3] != 'F')
	{
		goto out;
	}

	int i;
	Elf64_Shdr *Shdr = (Elf64_Shdr *)(Base + Header->e_shoff);
	char *Strtab = Base + Shdr[Header->e_shstrndx].sh_offset;

	for (i = 0; i < Header->e_shnum; i++)
	{
		char *Name = Strtab + Shdr[i].sh_name;
		if (!strncmp(Name, ".data", 6))
		{
			DsecSz = Shdr[i].sh_size;
		}
	}

out:
	munmap(Base, Statbuf.st_size);
	close(fd);
	return DsecSz;
}

void _runGC()
{
	NumGCTriggered++;

	size_t DataSecSz = getDataSecSz();
	unsigned char *DataStart;

	if (DataSecSz == -1)
	{
		DataStart = (unsigned char *)&etext;
	}
	else
	{
		DataStart = (unsigned char *)((char *)&edata - DataSecSz);
	}
	unsigned char *DataEnd = (unsigned char *)(&edata);

	/* scan global variables */
	scanRoots(DataStart, DataEnd);

	unsigned char *UnDataStart = (unsigned char *)(&edata);
	unsigned char *UnDataEnd = (unsigned char *)(&end);

	/* scan uninitialized global variables */
	scanRoots(UnDataStart, UnDataEnd);

	int Lvar;
	void *Base;
	size_t Size;
	pthread_attr_t Attr;

	int Ret = pthread_getattr_np(pthread_self(), &Attr);
	if (Ret != 0)
	{
		printf("Error getting stackinfo\n");
		return;
	}
	Ret = pthread_attr_getstack(&Attr, &Base, &Size);
	if (Ret != 0)
	{
		printf("Error getting stackinfo\n");
		return;
	}
	unsigned char *Bottom = (unsigned char *)(Base + Size);
	unsigned char *Top = (unsigned char *)&Lvar;
	/* skip GC stack frame */
	while (*((unsigned *)Top) != MAGIC_ADDR)
	{
		assert(Top < Bottom);
		Top++;
	}
	/* scan application stack */
	scanRoots(Top, Bottom);

	scanner();
	sweep();
}

static void checkAndRunGC(size_t Sz)
{
	static size_t TotalAlloc = 0;

	TotalAlloc += Sz;
	if (TotalAlloc < GC_THRESHOLD)
	{
		return;
	}
	TotalAlloc = 0;
	_runGC();
}

void printMemoryStats()
{
	printf("Num Bytes Allocated: %lld\n", NumBytesAllocated);
	printf("Num Bytes Freed: %lld\n", NumBytesFreed);
	printf("Num GC Triggered: %lld\n", NumGCTriggered);
}

// Size -> Array -> stores info for every page
// Normal allocations -> stores the number of bytes that are remaining or used
// Size[] -> 4096 -> Page is free

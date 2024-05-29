# Conservative Garbage Collection for C

## Overview
This system is a custom memory management system with an integrated conservative garbage collector (SafeGC) for the C programming language. Unlike managed languages such as Java, C lacks built-in memory safety features, allowing arbitrary typecasting and pointer arithmetic. This makes precise type information unavailable at runtime, complicating the implementation of garbage collection. SafeGC addresses this challenge by employing a conservative approach that does not require precise type information.

## Key Components

### Segments and Pages

#### Segments
Segments are large, contiguous blocks of memory, typically sized at 4GB. They serve as the primary units for memory allocation and management within the system. Each segment contains several pages and manages its memory via metadata pointers. Segments provide isolation between different memory regions, enhancing memory safety and allowing for efficient memory management.

#### Pages
Pages are smaller units within segments, typically sized at 4KB. They are the basic units of allocation for objects. Each page belongs to a segment and is managed by the segment's metadata. Pages provide granular control over memory allocation, facilitating efficient use of memory and reducing fragmentation.

#### Metadata
Metadata within a segment includes pointers that manage different stages of memory usage:

- **Free List Pointer**: Points to the head of the free list within the segment, indicating the available free space for allocation.
  
- **Allocated Blocks**: Tracks the allocated blocks within the segment, maintaining information such as the size and starting address of each allocated block. This metadata enables efficient allocation and deallocation of memory blocks.

- **Object Headers**: For each allocated block, an object header stores additional information, such as the size and type of the object, facilitating efficient memory management and garbage collection. Object headers enable the system to identify live objects during garbage collection and reclaim unreachable memory.

### Memory Allocation and Deallocation

#### Allocation
When a memory allocation request is made, the system searches for a suitable free block of memory within the segment. If a free block is found that can accommodate the requested size, it is allocated to the requesting application. If not, the system may request additional memory from the operating system by expanding the segment. The allocation process ensures efficient utilization of available memory while minimizing fragmentation.

#### Deallocation
Upon deallocation, the system marks the corresponding block as free in the segment's metadata, updating the free list pointer accordingly. This process ensures that the memory becomes available for subsequent allocations. Deallocation enables the system to reuse memory efficiently, reducing the likelihood of memory exhaustion and improving overall performance.

### Conservative Garbage Collection (SafeGC)

SafeGC employs a conservative approach to garbage collection, which does not require precise type information. This is particularly important in C, where explicit memory management allows for arbitrary typecasting and pointer arithmetic, making it challenging to accurately identify live objects. SafeGC performs garbage collection in two phases:

#### Mark Phase
The mark phase traverses through the allocated memory, marking live objects by identifying reachable memory regions from roots such as global variables, stack frames, and CPU registers. It conservatively treats any memory location that appears to be a pointer as a potential root. This phase ensures that all reachable objects are identified for retention.

#### Sweep Phase
The sweep phase iterates through all allocated memory blocks, freeing those that were not marked as live during the mark phase. This process reclaims memory occupied by unreachable objects, making it available for future allocations. The sweep phase ensures efficient memory utilization by removing unreferenced objects and preventing memory leaks.

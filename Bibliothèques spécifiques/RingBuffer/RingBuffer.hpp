// RingBuffer.hpp

#pragma once

#include <Arduino.h>

/***********************************************************************************
 Class RingBuffer<T, N>
************************************************************************************

  Circular Buffer (FIFO) data structure, with N elements of type T
*/

template <typename T, int N> class RingBuffer
{
  public:
    // Constructor
    RingBuffer() 
    {
      pStart = buffer;     // &buffer[0];
      pEnd = buffer + N-1; // &buffer[N-1];
      pHead = pStart; 
      pTail = pStart; 
      size = 0;
    }
    
    // Number of elements
    int Size() {return size;}

    // Operations
    void Push(T *t) // Before test if Size() < N
    {
      if (size == N) return;
      *pTail++ = *t;
      if (pTail > pEnd) pTail = pStart; 
      size++;
    }

    void Pop(T *t) // Before test if Size() > 0
    {
      if (size == 0) return;
      *t = *pHead++;
      if (pHead > pEnd) pHead = pStart; 
      size--;
    }

    void Get(T *t, int n) // Before test if 0 <= n <= Size()
    {
      if (n < 0 || n > size) return;
      T *p = pHead + n;
      if (p > pEnd) p -= N;
      *t = *p;
    }
  
  private:
    T buffer[N];
    T *pHead, *pTail, *pStart, *pEnd;
    int size;
};

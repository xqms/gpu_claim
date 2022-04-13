// Priority queue for waiting jobs
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#ifndef PRIORITY_QUEUE_H
#define PRIORITY_QUEUE_H

#include <deque>

#include "protocol.h"

class PriorityQueue : private std::deque<Job>
{
public:
    using std::deque<Job>::operator[];
    using std::deque<Job>::size;
    using std::deque<Job>::empty;
    using std::deque<Job>::front;
    using std::deque<Job>::pop_front;

    void enqueue(Job&& job);
    void update();

    void remove(int pid);
};

#endif

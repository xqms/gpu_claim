// Priority queue for waiting jobs
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#include "priority_queue.h"

#include <ranges>

void PriorityQueue::enqueue(Job&& job)
{
    push_back(std::move(job));
}

void PriorityQueue::remove(int pid)
{
    auto it = std::ranges::find_if(static_cast<std::deque<Job>&>(*this), [&](auto& j){
        return j.pid == pid;
    });
    if(it != end())
        erase(it);
}

void PriorityQueue::update()
{
}


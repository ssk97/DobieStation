#include "gsslave.hpp"
#include "gsthread.hpp"

GraphicsSynthesizerSlave::GraphicsSynthesizerSlave(GraphicsSynthesizerThread *gs)
{
    remaining = 0;
    error_report = nullptr;
    fifo = new gs_slave_fifo();
    slave_thread = std::thread(&event_loop, gs);
}

void GraphicsSynthesizerSlave::event_loop(GraphicsSynthesizerThread *gs, GraphicsSynthesizerSlave *s)
{
    while (true)
    {
        gs_slave_command data;
        bool available = s->fifo->pop(data);
        switch (data.type)
        {
            case slave_tri:
                break;
            case slave_sprite:
                break;
            case slave_die:
                return;
        }
    }
}
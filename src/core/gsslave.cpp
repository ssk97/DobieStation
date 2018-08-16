#include "gsslave.hpp"
#include "gsthread.hpp"

GraphicsSynthesizerSlave::GraphicsSynthesizerSlave(GraphicsSynthesizerThread *gs)
{
    remaining = 0;
    error_report = nullptr;
    slave_thread = std::thread(&event_loop, gs);
    //fifo is implicitly initialized
    //actually so are remaining and error_report, but w/e
}

void GraphicsSynthesizerSlave::event_loop(GraphicsSynthesizerThread *gs, GraphicsSynthesizerSlave *s)
{
    try
    {
        while (true)
        {
            gs_slave_command data;
            bool available = s->fifo.pop(data);
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
    catch (Emulation_error &e)
    {
        GS_return_message_payload return_payload;
        char* copied_string = new char[ERROR_STRING_MAX_LENGTH];
        strncpy(copied_string, e.what(), ERROR_STRING_MAX_LENGTH);
        s->error_report = copied_string;
    }
}
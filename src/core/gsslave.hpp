#ifndef GSSLAVE_HPP
#define GSSLAVE_HPP
#include "circularFIFO.hpp"
#include <cstdint>
#include <thread>
class GraphicsSynthesizerThread;

union gs_slave_payload
{
    struct
    {
        int32_t scanline;
    } sprite_payload;
    struct
    {
        int32_t block_x, block_y;
    } tri_payload;
    struct
    {
        uint8_t BLANK;
    } no_payload;//C++ doesn't like the empty struct
};

union gs_slave_shared_data
{
    struct
    {

        float pix_t;
        int32_t pix_v;
        float pix_s_init;
        int32_t pix_u_init;

        float pix_t_step;
        int32_t pix_v_step;
        float pix_s_step;
        int32_t pix_u_step;
    } sprite_data;
    struct
    {

    } tri_data;
};

enum gs_slave_type: uint8_t
{
    slave_die, slave_sprite, slave_tri
};

struct gs_slave_command
{
    gs_slave_type type;
    gs_slave_payload payload;
};

typedef CircularFifo<gs_slave_command, 1024*2> gs_slave_fifo;

class GraphicsSynthesizerSlave
{
    private:
        std::thread slave_thread;
        std::atomic_int16_t remaining;
        std::atomic<char*> error_report;
        gs_slave_fifo fifo;
    public:
        static void event_loop(GraphicsSynthesizerThread *gs, GraphicsSynthesizerSlave *s);
        bool check_complete(); //also does an error check
        void send(gs_slave_command msg);
        GraphicsSynthesizerSlave(GraphicsSynthesizerThread *gs);
        ~GraphicsSynthesizerSlave();
};

#endif // GSSLAVE_HPP
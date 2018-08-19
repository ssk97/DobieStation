#ifndef GSSLAVE_HPP
#define GSSLAVE_HPP
#include "circularFIFO.hpp"
#include "gsshared.hpp"
#include <cstdint>
#include <thread>
class GraphicsSynthesizerThread;

union gs_slave_payload
{
    struct
    {
        int32_t scanline, i;
    } sprite_payload;
    struct
    {
        int32_t x_block, y_block;
        int32_t w1_row, w2_row, w3_row;
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
        int32_t min_x, max_x;
        Vertex v1, v2;

        float pix_t_init;
        int32_t pix_v_init;
        float pix_s_init;
        int32_t pix_u_init;

        float pix_t_step;
        int32_t pix_v_step;
        float pix_s_step;
        int32_t pix_u_step;
    } sprite_data;
    struct
    {
        Vertex v1, v2, v3;
        int32_t A12, B12, A23, B23, A31, B31;
        int32_t divider;
    } tri_data;
};

enum gs_slave_type: uint8_t
{
    slave_die, slave_sprite, slave_tri, slave_sleep
};

struct gs_slave_command
{
    gs_slave_type type;
    gs_slave_payload payload;
};

typedef CircularFifo<gs_slave_command, 1024*32> gs_slave_fifo;

class GraphicsSynthesizerSlave
{
    private:
        std::thread slave_thread;
        std::atomic_int16_t remaining;
        std::atomic<char*> error_report;
        gs_slave_fifo fifo;
        GraphicsSynthesizerThread *gs;

        void sprite(gs_slave_payload spr_data);
        void triangle(gs_slave_payload tri_data);
    public:
        GraphicsSynthesizerSlave(GraphicsSynthesizerThread *gs);
        static void event_loop( GraphicsSynthesizerSlave *s);

        bool check_complete(); //also does an error check
        void send(gs_slave_command msg);
        void kill();

        friend GraphicsSynthesizerThread;
};

#endif // GSSLAVE_HPP
#include "gsslave.hpp"
#include "gsthread.hpp"

GraphicsSynthesizerSlave::GraphicsSynthesizerSlave(GraphicsSynthesizerThread *this_gs)
{
    remaining = 0;
    error_report = nullptr;
    gs = this_gs;
    slave_thread = std::thread(&event_loop, this);
    //fifo is implicitly initialized
    //actually so are remaining and error_report, but w/e
}

void GraphicsSynthesizerSlave::send(gs_slave_command command)
{
    remaining.fetch_add(1, std::memory_order_consume);
    fifo.push(command);
}

bool GraphicsSynthesizerSlave::check_complete()
{
    if (error_report != nullptr)
    {
        Errors::die(error_report);
    }
    return (remaining.load(std::memory_order_consume) == 0);
}


void GraphicsSynthesizerSlave::event_loop(GraphicsSynthesizerSlave *s)
{
    try
    {
        bool sleep = false;
        while (true)
        {
            gs_slave_command data;
            bool available = s->fifo.pop(data);
            if (available)
            {
                sleep = false;
                switch (data.type)
                {
                    case slave_tri:
                        break;
                    case slave_sprite:
                        s->sprite(data.payload.sprite_payload.scanline, data.payload.sprite_payload.i);
                        break;
                    case slave_sleep:
                        sleep = true;
                        break;
                    case slave_die:
                        return;
                }
                s->remaining.fetch_sub(1, std::memory_order_release);
            }
            else if (sleep)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            else
                std::this_thread::yield();
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

void GraphicsSynthesizerSlave::sprite(int32_t y, int32_t i)
{
    auto shared = &(gs->slave_shared_data.sprite_data);


    bool tmp_tex = gs->PRIM.texture_mapping;
    bool tmp_uv = !gs->PRIM.use_UV;

    float pix_t = shared->pix_t_init + (shared->pix_t_step * i);
    uint32_t pix_v = shared->pix_v_init + (shared->pix_v_step*i);

    float pix_s = shared->pix_s_init;
    uint32_t pix_u = shared->pix_u_init;

    RGBAQ_REG vtx_color, tex_color;
    vtx_color = gs->vtx_queue[0].rgbaq;

    for (int32_t x = shared->min_x; x < shared->max_x; x += 0x10)
    {
        if (tmp_tex)
        {
            if (tmp_uv)
            {
                pix_v = (pix_t * gs->current_ctx->tex0.tex_height) * 16;
                pix_u = (pix_s * gs->current_ctx->tex0.tex_width) * 16;
                gs->tex_lookup(pix_u, pix_v, vtx_color, tex_color);
            }
            else
                gs->tex_lookup(pix_u >> 16, pix_v >> 16, vtx_color, tex_color);
            gs->draw_pixel(x, y, shared->v2.z, tex_color, gs->PRIM.alpha_blend);
        }
        else
        {
            gs->draw_pixel(x, y, shared->v2.z, vtx_color, gs->PRIM.alpha_blend);
        }
        pix_s += shared->pix_s_step;
        pix_u += shared->pix_u_step;
    }
}
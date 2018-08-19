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

void GraphicsSynthesizerSlave::kill()
{
    gs_slave_payload p;
    p.no_payload = {};
    send({ slave_die, p });
    slave_thread.join();
}


void GraphicsSynthesizerSlave::event_loop(GraphicsSynthesizerSlave *s)
{
    try
    {
        bool sleep = false;
        uint32_t local_count = 0;
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
                        s->triangle(data.payload);
                        break;
                    case slave_sprite:
                        s->sprite(data.payload);
                        break;
                    case slave_sleep:
                        sleep = true;
                        break;
                    case slave_die:
                        return;
                }
                local_count++;
            }
            else
            {
                if (local_count > 0)
                {
                    s->remaining.fetch_sub(local_count, std::memory_order_release);
                    local_count = 0;
                }

                if (sleep)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                else
                    std::this_thread::yield();
            }
        }
    }
    catch (Emulation_error &e)
    {
        char* copied_string = new char[ERROR_STRING_MAX_LENGTH];
        strncpy(copied_string, e.what(), ERROR_STRING_MAX_LENGTH);
        s->error_report = copied_string;
    }
}

void GraphicsSynthesizerSlave::sprite(gs_slave_payload spr_data)
{
    auto shared = &(gs->slave_shared_data.sprite_data);
    
    auto data = spr_data.sprite_payload;
    auto y = data.scanline;
    auto i = data.i;

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

void GraphicsSynthesizerSlave::triangle(gs_slave_payload tri_data)
{
    const int32_t BLOCKSIZE = 1 << 8; // WARNING: must be identical to gsthread's render_triangle BLOCKSIZE

    auto shared = &(gs->slave_shared_data.tri_data);

    auto data = tri_data.tri_payload;
    auto y_block = data.y_block;
    auto x_block = data.x_block;
    auto w1_row = data.w1_row;
    auto w2_row = data.w2_row;
    auto w3_row = data.w3_row;


    bool tmp_tex = gs->PRIM.texture_mapping;
    bool tmp_uv = !gs->PRIM.use_UV;
    RGBAQ_REG vtx_color, tex_color;

    auto divider = shared->divider;
    auto v1 = shared->v1;
    auto v2 = shared->v2;
    auto v3 = shared->v3;
    auto A12 = shared->A12;
    auto B12 = shared->B12;
    auto A23 = shared->A23;
    auto B23 = shared->B23;
    auto A31 = shared->A31;
    auto B31 = shared->B31;

    auto tex_width = gs->current_ctx->tex0.tex_width;
    auto tex_height = gs->current_ctx->tex0.tex_height;

    bool alpha_blend = gs->PRIM.alpha_blend;

    for (int32_t y = y_block; y < y_block + BLOCKSIZE; y += 0x10)
    {
        int32_t w1 = w1_row;
        int32_t w2 = w2_row;
        int32_t w3 = w3_row;
        for (int32_t x = x_block; x < x_block + BLOCKSIZE; x += 0x10)
        {
            //Is inside triangle?
            if ((w1 | w2 | w3) >= 0)
            {
                //Interpolate Z
                double z = (double)v1.z * w1 + (double)v2.z * w2 + (double)v3.z * w3;
                z /= divider;

                //Gourand shading calculations
                float r = (float)v1.rgbaq.r * w1 + (float)v2.rgbaq.r * w2 + (float)v3.rgbaq.r * w3;
                float g = (float)v1.rgbaq.g * w1 + (float)v2.rgbaq.g * w2 + (float)v3.rgbaq.g * w3;
                float b = (float)v1.rgbaq.b * w1 + (float)v2.rgbaq.b * w2 + (float)v3.rgbaq.b * w3;
                float a = (float)v1.rgbaq.a * w1 + (float)v2.rgbaq.a * w2 + (float)v3.rgbaq.a * w3;
                float q = v1.rgbaq.q * w1 + v2.rgbaq.q * w2 + v3.rgbaq.q * w3;
                vtx_color.r = r / divider;
                vtx_color.g = g / divider;
                vtx_color.b = b / divider;
                vtx_color.a = a / divider;
                vtx_color.q = q / divider;

                if (tmp_tex)
                {
                    uint32_t u, v;
                    if (tmp_uv)
                    {
                        float s, t, q;
                        s = v1.s * w1 + v2.s * w2 + v3.s * w3;
                        t = v1.t * w1 + v2.t * w2 + v3.t * w3;
                        q = v1.rgbaq.q * w1 + v2.rgbaq.q * w2 + v3.rgbaq.q * w3;

                        //We don't divide s and t by "divider" because dividing by Q effectively
                        //cancels that out
                        s /= q;
                        t /= q;
                        u = (s * tex_width) * 16.0;
                        v = (t * tex_height) * 16.0;
                    }
                    else
                    {
                        float temp_u = (float)v1.uv.u * w1 + (float)v2.uv.u * w2 + (float)v3.uv.u * w3;
                        float temp_v = (float)v1.uv.v * w1 + (float)v2.uv.v * w2 + (float)v3.uv.v * w3;
                        temp_u /= divider;
                        temp_v /= divider;
                        u = (uint32_t)temp_u;
                        v = (uint32_t)temp_v;
                    }
                    gs->tex_lookup(u, v, vtx_color, tex_color);
                    gs->draw_pixel(x, y, (uint32_t)z, tex_color, alpha_blend);
                }
                else
                {
                    gs->draw_pixel(x, y, (uint32_t)z, vtx_color, alpha_blend);
                }
            }
            //Horizontal step
            w1 += A23 << 4;
            w2 += A31 << 4;
            w3 += A12 << 4;
        }
        //Vertical step
        w1_row += B23 << 4;
        w2_row += B31 << 4;
        w3_row += B12 << 4;
    }
}
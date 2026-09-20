#include <vircon.h>

/* Very Simple Sample */

int main(void)
{
    int frames = 0;

    for (;;) {
        clear_screen(0xFF202040);
        print_at(180, 150, "Hello from wasm2vircon!");

        if (frames & 32)
            print_at(220, 190, "C -> Wasm -> Vircon32");

        ++frames;
        end_frame();
    }
}

#ifndef MUSASHI_GLUE_H
#define MUSASHI_GLUE_H
int  mus_stopped(void);
void mus_clear_stopped(void);
void mus_set_stopped(void);
int  mus_base_cycles(unsigned opcode);
#endif

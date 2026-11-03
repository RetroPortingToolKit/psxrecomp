#include <assert.h>
#include <stdio.h>
#include "digital_pad_snapshot.h"
int main(void) {
 PSXResolvedDigitalPad s={0};PSXModDigitalPadReader r={sizeof r};uint32_t out=0;
 digital_pad_history_reset(&s,0xffff);assert(!digital_pad_reader_read(&r,&s,0,0,&out));
 /* A complete short tap between original game reads is not lost. */
 digital_pad_history_record(&s,0xffdf);digital_pad_history_record(&s,0xffff);
 assert(digital_pad_reader_read(&r,&s,1,0,&out) && out==0x20);
 assert(digital_pad_reader_read(&r,&s,1,0,&out) && out==0x20); /* same frame */
 assert(digital_pad_reader_read(&r,&s,2,0x20,&out) && !out); /* delayed packet cannot replay it */
 assert(digital_pad_reader_read(&r,&s,3,0,&out) && !out);
 /* Held input is never turned into repeated presses; release is prompt. */
 digital_pad_history_record(&s,0xffbf);
 assert(digital_pad_reader_read(&r,&s,4,0,&out) && out==0x40);
 for(unsigned f=5;f<10;++f) {
  digital_pad_history_record(&s,0xffbf);
  assert(digital_pad_reader_read(&r,&s,f,0x40,&out) && out==0x40);
 }
 digital_pad_history_record(&s,0xffff);
 assert(digital_pad_reader_read(&r,&s,10,0x40,&out) && !out);
 /* Two captured taps are separated into two presses and two releases. */
 for(int i=0;i<2;++i) {digital_pad_history_record(&s,0x7fff);digital_pad_history_record(&s,0xffff);}
 for(unsigned f=11;f<15;++f) {
  assert(digital_pad_reader_read(&r,&s,f,0,&out));assert(out==((f&1)?0x8000:0));
 }
 /* Scene/load gaps and device changes never replay accumulated actions. */
 digital_pad_history_record(&s,0xffdf);digital_pad_history_record(&s,0xffff);
 assert(!digital_pad_reader_read(&r,&s,20,0,&out));
 assert(digital_pad_reader_read(&r,&s,21,0,&out) && !out);
 digital_pad_history_reset(&s,0xffff);assert(!digital_pad_reader_read(&r,&s,22,0,&out));
 /* All 16 button bits, including sign bit, remain independent. */
 for(unsigned bit=0;bit<16;++bit) {
  digital_pad_history_record(&s,(uint16_t)~(1u<<bit));
  assert(digital_pad_reader_read(&r,&s,23+2*bit,0,&out) && out==(1u<<bit));
  digital_pad_history_record(&s,0xffff);
  assert(digital_pad_reader_read(&r,&s,24+2*bit,0,&out) && !out);
 }
 /* Counter wrap and unsupported burst backlog fail safely without overflow. */
 digital_pad_reader_reset(&r);s.press[0]=0xffffffffu;
 assert(!digital_pad_reader_read(&r,&s,100,0,&out));
 digital_pad_history_record(&s,0xfffe);digital_pad_history_record(&s,0xffff);
 assert(digital_pad_reader_read(&r,&s,101,0,&out) && out==1);
 digital_pad_reader_reset(&r);assert(!digital_pad_reader_read(&r,&s,200,0,&out));
 s.press[0]+=10;assert(!digital_pad_reader_read(&r,&s,201,0,&out));
 assert(digital_pad_reader_read(&r,&s,202,0,&out) && !out);
 puts("PASS digital history: short taps, edges, repeat reads, rapid taps, gaps, reset, wrap and bounded backlog");
}

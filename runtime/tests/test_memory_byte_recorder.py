"""Observe byte accesses with the same opt-in recorder exclusions as word/half.

This tests the memory-to-recorder feed only. It neither enables data-shard
replay by default nor establishes the safety of temporal replay.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
import test_memory_debug_trace_guard as fixture

DRIVER = r'''
#include "harness_common.h"
#include <stdio.h>
uint8_t *memory_get_ram_ptr(void);
void psx_write_byte(uint32_t,uint8_t);
void psx_write_half(uint32_t,uint16_t);
void psx_write_word(uint32_t,uint32_t);
extern unsigned byte_reads,byte_writes,dma_poison,observed_width;
extern uint32_t observed_address;
extern int fixture_exception;
static unsigned failures;
#define CHECK(c,msg) do{if(!(c)){fprintf(stderr,"FAIL: %s\n",msg);++failures;}}while(0)
int main(void){
 uint8_t *ram=memory_get_ram_ptr();ram[0x1000]=0x5a;
 g_ds_recording=0;
 CHECK(psx_read_byte(0x80001000)==0x5a && byte_reads==0,"disabled recorder is inert");
 g_ds_recording=1;
 CHECK(psx_read_byte(0x80001000)==0x5a,"captured byte read preserves RAM value");
 CHECK(byte_reads==1 && observed_width==1 && observed_address==0x80001000,"byte read enters the read set with width and alias");
 psx_write_byte(0xa0001000,0x63);
 CHECK(ram[0x1000]==0x63,"captured byte store preserves RAM effect");
 CHECK(byte_writes==1 && observed_width==1 && observed_address==0xa0001000,"byte store enters the write set with width and alias");
 unsigned reads=byte_reads,writes=byte_writes;
 fixture_exception=1;psx_read_byte(0x1000);psx_write_byte(0x1000,0x44);
 CHECK(byte_reads==reads && byte_writes==writes && ram[0x1000]==0x44,"ISR accesses remain live and outside capture");
 fixture_exception=0;g_dma_exec_depth=1;
 psx_read_byte(0x1000);psx_write_byte(0x1000,0x45);
 CHECK(byte_reads==reads && byte_writes==writes && dma_poison==1,"DMA excludes byte reads and poisons byte writes");
 g_dma_exec_depth=0;g_ls_mode=2;
 CHECK(psx_read_byte(0x1000)==0,"replay read uses its supplied lockstep value");
 psx_write_byte(0x1000,0x46);
 CHECK(byte_reads==reads && byte_writes==writes && ram[0x1000]==0x45,"lockstep replay does not capture or apply another guest store");
 g_ls_mode=0;g_ds_recording=0;
 psx_read_byte(0x1000);psx_write_byte(0x1000,0x47);
 CHECK(byte_reads==reads && byte_writes==writes && ram[0x1000]==0x47,"turning capture off preserves normal byte behavior");
 /* Existing widths remain independently observable, not byte aliases. */
 g_ds_recording=1;
 psx_read_half(0x1000);CHECK(observed_width==2,"halfword read width remains two");
 psx_read_word(0x1000);CHECK(observed_width==4,"word read width remains four");
 psx_write_half(0x1000,0x1234);CHECK(observed_width==2,"halfword store width remains two");
 psx_write_word(0x1000,0x12345678);CHECK(observed_width==4,"word store width remains four");
 return failures?1:0;
}
'''

def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--cc',required=True)
 parser.add_argument('--memory-source',type=Path,default=fixture.MEMORY_C)
 args=parser.parse_args()
 fixture.DRIVER_C=DRIVER
 fixture.STUBS_C=fixture.STUBS_C.replace('int psx_get_in_exception(void) { return 0; }','int fixture_exception; int psx_get_in_exception(void) { return fixture_exception; }')
 fixture.STUBS_C=fixture.STUBS_C.replace('void ds_note_read(uint32_t addr, uint8_t width) { (void)addr; (void)width; }',
  'unsigned byte_reads,byte_writes,dma_poison,observed_width; uint32_t observed_address; void ds_note_read(uint32_t addr,uint8_t width){++byte_reads;observed_width=width;observed_address=addr;}')
 fixture.STUBS_C=fixture.STUBS_C.replace('void ds_note_write(uint32_t addr, uint8_t width) { (void)addr; (void)width; }',
  'void ds_note_write(uint32_t addr,uint8_t width){++byte_writes;observed_width=width;observed_address=addr;}')
 fixture.STUBS_C=fixture.STUBS_C.replace('void ds_note_dma_write(void) {}','void ds_note_dma_write(void){++dma_poison;}')
 code=0
 with tempfile.TemporaryDirectory(prefix='byte-recorder-') as directory:
  for optimization in ('-O0','-O2'):
   work=Path(directory)/optimization[1:];work.mkdir()
   exe=fixture.build(args.cc,work,args.memory_source,'byte_recorder',('PSX_NO_DEBUG_TOOLS',),'trace_empty.c',optimization)
   measured=subprocess.run([str(exe)],text=True,encoding='utf-8',errors='replace',capture_output=True)
   print(optimization,measured.stdout,measured.stderr,sep='\n')
   code=code or measured.returncode
 return code
if __name__=='__main__':raise SystemExit(main())

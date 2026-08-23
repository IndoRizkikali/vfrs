# VFRS - Virtual Frame Relay Switch
# Makefile for MSYS2 UCRT64 with GCC

CC = gcc
CFLAGS = -Wall -Wextra -O2 -static -D_WIN32_WINNT=0x0A00 -D_GNU_SOURCE -I.
LDFLAGS = -static -lws2_32 -lwinmm -ladvapi32

TARGET = vfrs.exe

# Source files
SRC = vfrs_main.c logger.c config.c \
      fr_switching/fr_frame.c fr_switching/fr_switch.c fr_switching/svc_routing_common.c \
      ports/port_common.c ports/port_queue.c ports/port_udp.c ports/port_tcp.c \
      ports/port_serial.c ports/port_pipe.c ports/lapf/port_lapf.c ports/svc_numbering/svc_numbering.c \
      ports/pcap/port_pcap.c \
      pvc/pvc_lmi_common.c pvc/pvc_lmi_ansi.c pvc/pvc_lmi_gof.c pvc/pvc_lmi_q933a.c \
      pvc/pvc_mcast_uni.c pvc/pvc_mcast_nni.c \
      congestion_mgnt/cgst_mgnt.c congestion_mgnt/cgst_cllm.c \
      svc_signalling/svc_sig_common.c svc_signalling/svc_sig_iel.c svc_signalling/svc_sig_iep.c svc_signalling/svc_sig_uni.c svc_signalling/svc_sig_nni.c

# Object files
OBJ = obj/vfrs_main.o obj/logger.o obj/config.o \
      obj/fr_switching/fr_frame.o obj/fr_switching/fr_switch.o obj/fr_switching/svc_routing_common.o \
      obj/ports/port_common.o obj/ports/port_queue.o obj/ports/port_udp.o obj/ports/port_tcp.o \
      obj/ports/port_serial.o obj/ports/port_pipe.o obj/ports/lapf/port_lapf.o obj/ports/svc_numbering/svc_numbering.o \
      obj/ports/pcap/port_pcap.o \
      obj/pvc/pvc_lmi_common.o obj/pvc/pvc_lmi_ansi.o obj/pvc/pvc_lmi_gof.o obj/pvc/pvc_lmi_q933a.o \
      obj/pvc/pvc_mcast_uni.o obj/pvc/pvc_mcast_nni.o \
      obj/congestion_mgnt/cgst_mgnt.o obj/congestion_mgnt/cgst_cllm.o \
      obj/svc_signalling/svc_sig_common.o obj/svc_signalling/svc_sig_iel.o obj/svc_signalling/svc_sig_iep.o obj/svc_signalling/svc_sig_uni.o obj/svc_signalling/svc_sig_nni.o

all: dirs $(TARGET)

dirs:
	mkdir -p obj obj/fr_switching obj/ports obj/ports/lapf obj/ports/svc_numbering obj/ports/pcap obj/pvc obj/congestion_mgnt obj/svc_signalling

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJ): | dirs

obj/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

debug: CFLAGS = -Wall -Wextra -g -O0 -fno-omit-frame-pointer -D_WIN32_WINNT=0x0A00 -D_GNU_SOURCE -I.
debug: LDFLAGS = -lws2_32 -lwinmm -ladvapi32
debug: clean dirs $(TARGET)

check: debug
	bash ./tests/run_pipe_loopback_test.sh

test: all
	$(CC) $(CFLAGS) -o tests/ie_test.exe tests/ie_test.c obj/logger.o obj/fr_switching/fr_frame.o obj/congestion_mgnt/cgst_cllm.o obj/svc_signalling/svc_sig_iel.o obj/svc_signalling/svc_sig_iep.o
	./tests/ie_test.exe
	python tests/svc_compliance_test.py

clean:
	rm -rf obj $(TARGET) tests/ie_test.exe

.PHONY: all clean dirs debug check test
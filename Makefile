# ---------------------------------------------------------------------------
# eicd - Edge Industrial Communication Daemon
#
# Targets:
#   make            build the daemon (build/eicd -> ./eicd)
#   make clean      remove objects and the binary
#   make install    install binary + example config (honours DESTDIR/PREFIX)
#   make uninstall  remove the installed binary (config is kept)
# ---------------------------------------------------------------------------

TARGET   := eicd
SRCDIR   := src
INCDIR   := include
OBJDIR   := build

CC       ?= gcc
OPT      ?= -O2
CFLAGS   ?= $(OPT) -g
CFLAGS   += -std=c11 -D_GNU_SOURCE -I$(INCDIR) \
            -Wall -Wextra -Wshadow -Wpointer-arith -Wformat=2 \
            -Wstrict-prototypes -Wmissing-prototypes \
            -fno-strict-aliasing
LDLIBS   += -pthread -lm

SRCS := $(sort $(wildcard $(SRCDIR)/*.c))
OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

PREFIX     ?= /usr/local
SYSCONFDIR ?= /etc

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/sbin
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/sbin/$(TARGET)
	install -d $(DESTDIR)$(SYSCONFDIR)/eicd
	@[ -f $(DESTDIR)$(SYSCONFDIR)/eicd/eicd.conf ] || \
	    install -m 0640 etc/eicd.conf.example $(DESTDIR)$(SYSCONFDIR)/eicd/eicd.conf
	@[ -f $(DESTDIR)$(SYSCONFDIR)/eicd/points.csv ] || \
	    install -m 0640 etc/points.csv.example $(DESTDIR)$(SYSCONFDIR)/eicd/points.csv
	install -d $(DESTDIR)/var/lib/eicd
	@echo "installed. systemd unit: packaging/eicd.service"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/sbin/$(TARGET)

clean:
	rm -rf $(OBJDIR) $(TARGET)

-include $(DEPS)

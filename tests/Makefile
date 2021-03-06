
all::

LD = $(CC)

uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo no')

CFLAGS = -g -O2 -Wall
LIBS   = -L.. -lgit2 -lz
OS     = unix

VFLAGS = -q --error-exitcode=1 --leak-check=yes --suppressions=../tests.supp

CRYPTO_LIB = -lcrypto

EXTRA_LIBS =

HDRS = $(wildcard ../src/*.h)
PUBLIC_HEADERS = $(wildcard ../src/git/*.h)
HDRS += $(PUBLIC_HEADERS)

TEST_OBJ = $(patsubst %.c,%.o,\
           $(wildcard t[0-9][0-9][0-9][0-9]-*.c))
TEST_EXE = $(patsubst %.o,%.exe,$(TEST_OBJ))
TEST_RUN = $(patsubst %.exe,%.run,$(TEST_EXE))
TEST_VAL = $(patsubst %.exe,%.val,$(TEST_EXE))

ifdef TEST_COVERAGE
	EXTRA_LIBS += -O0 -lgcov
endif

ifndef NO_OPENSSL
	EXTRA_LIBS += $(CRYPTO_LIB)
endif

BASIC_CFLAGS := -I../src

ALL_CFLAGS = $(CFLAGS) $(BASIC_CFLAGS)
ALL_LIBS = $(LIBS) $(EXTRA_LIBS)

all:: $(TEST_RUN)

clean:
	rm -f *.o *.exe *.toc
	rm -rf trash-*.exe
	rm -f *~

test: $(TEST_RUN)

valgrind: $(TEST_VAL)

.c.o:
	$(CC) $(ALL_CFLAGS) -c $< -o $@

T_HDR         = test_lib.h test_helpers.h
T_LIB         = test_lib.o test_helpers.o
T_MAIN_C      = test_main.c

$(T_LIB):    $(T_HDR) $(HDRS)
$(TEST_OBJ): $(T_HDR) $(HDRS)

$(patsubst %.exe,%.toc,$(TEST_EXE)): %.toc: %.c
	grep BEGIN_TEST $< >$@+
	mv $@+ $@

$(TEST_OBJ): %.o: %.c
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(patsubst %.exe,%_main.o,$(TEST_EXE)): %_main.o: $(HDRS)
$(patsubst %.exe,%_main.o,$(TEST_EXE)): %_main.o: $(T_MAIN_C)
$(patsubst %.exe,%_main.o,$(TEST_EXE)): %_main.o: %.toc
	$(CC) $(ALL_CFLAGS) '-DTEST_TOC="$<"' \
		-c $(T_MAIN_C) \
		-o $@

$(TEST_EXE): %.exe: $(T_LIB)
$(TEST_EXE): %.exe: %.o %_main.o
	$(LD) -o $@ \
		$(patsubst %.exe,%_main.o,$@) \
		$(patsubst %.exe,%.o,$@) \
		$(T_LIB) $(ALL_LIBS)

$(TEST_RUN): %.run: %.exe
	@t=trash-$(<F) && \
	 mkdir $$t && \
	 if (cd $$t && ../$<); \
	  then rm -rf $$t; \
	  else rmdir $$t; exit 1; \
	 fi

$(TEST_VAL): %.val: %.exe
	@t=trash-$(<F) && \
	 mkdir $$t && \
	 if (cd $$t && valgrind $(VFLAGS) ../$<); \
	  then rm -rf $$t; \
	  else rmdir $$t; exit 1; \
	 fi

.PHONY: all
.PHONY: clean
.PHONY: test $(TEST_RUN)
.PHONY: $(TEST_VAL)

INSTALL		= /usr/lib/maxkernel
RELEASE		= BETA

MODULES		= calibration controls map motionmodel webcam ssc gps network maxpod jpeg usrf_reader bot_blackbox
#WORKING_ON	= roomba serialpwm

DEFINES		= -D_GNU_SOURCE -D$(RELEASE) -DRELEASE="\"$(RELEASE)\"" -DINSTALL="\"$(INSTALL)\""


export INSTALL
export RELEASE

.PHONY: prepare all install clean rebuild depend

all: prepare
	#all: prereq body
	$(foreach module,$(MODULES),python makefile.gen.py --module $(module) --defines '$(DEFINES)' >$(module)/Makefile && $(MAKE) -C $(module) depend &&) true
	( $(foreach module,$(MODULES), echo "In module $(module)" >>buildlog && $(MAKE) -C $(module) all 2>>buildlog &&) true ) || ( cat buildlog && false )
	( echo "Done (success)" >>buildlog && cat buildlog )

install:
	mkdir -p $(INSTALL) $(INSTALL)/modules /usr/include/maxkernel
	$(foreach module,$(MODULES),$(MAKE) -C $(module) install && ( [ -e $(module)/install.part.bash ] && ( cd $(module) && bash install.part.bash ) || true ) &&) true
	
clean:
	( $(foreach module,$(MODULES),$(MAKE) -C $(module) clean; ( cd $(module) && bash clean.part.bash );) ) || true
	rm -f $(foreach module,$(MODULES),$(module)/Makefile)
	
rebuild: clean all

depend:
	# TODO - finish me

prepare:
	echo "Module build log ==----------------------== [$(shell date)]" >buildlog


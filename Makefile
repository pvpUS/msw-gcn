#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

#---------------------------------------------------------------------------------
# PLATFORM picks the console. Both builds come out of this one tree:
#
#   make                 -> msw-gcn.dol, for a GameCube (or Dolphin, or a disc)
#   make wii             -> msw-wii.dol, for the Homebrew Channel
#
# They differ in three places and nowhere else: the toolchain rules, the
# libraries, and source/pad.c. wii_rules defines HW_RVL, which is what every
# platform #ifdef in source/ keys off.
#---------------------------------------------------------------------------------
PLATFORM	?=	gamecube
export PLATFORM

ifeq ($(PLATFORM),wii)
include $(DEVKITPPC)/wii_rules
TARGET		:=	msw-wii
BUILD		:=	build_wii
else
ifeq ($(PLATFORM),gamecube)
include $(DEVKITPPC)/gamecube_rules
TARGET		:=	msw-gcn
BUILD		:=	build
else
$(error PLATFORM must be 'gamecube' or 'wii', not '$(PLATFORM)')
endif
endif

#---------------------------------------------------------------------------------
# SOURCES is a list of directories containing source code
# INCLUDES is a list of directories containing extra header files
#---------------------------------------------------------------------------------
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------

CFLAGS		= -g -O2 -Wall $(MACHDEP) $(INCLUDE)
CXXFLAGS	= $(CFLAGS)

LDFLAGS		= -g $(MACHDEP) -Wl,-Map,$(notdir $@).map

#---------------------------------------------------------------------------------
# any extra libraries we wish to link with the project
#---------------------------------------------------------------------------------
# On GameCube, -lbba is the Broadband Adapter's lwIP stack: if_config and the
# net_* socket calls source/net.c uses live there, not in libogc (which only
# declares them). On Wii the very same API is libogc's own, over the console's
# network stack -- which is why net.c is identical on both -- and the extra
# libraries are WPAD's, for the Classic Controller path in pad.c.
ifeq ($(PLATFORM),wii)
LIBS	:=	-lwiiuse -lbte -logc -lm
else
LIBS	:=	-lbba -logc -lm
endif

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:=

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

#---------------------------------------------------------------------------------
# automatically build a list of object files for our project
#---------------------------------------------------------------------------------
CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
# Only the two extensions with a bin2o rule below. Deliberately not *.* --
# data/ also holds the build inputs the asset tools read and write
# (blockids.txt, materials.txt, and gxtexconv's intermediate atlas.png/atlas.h),
# and a wildcard over all of them asks make for a nonexistent <file>.o rule.
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.mworld) $(wildcard $(dir)/*.tpl)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(sFILES:.s=.o) $(SFILES:.S=.o)
export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES := $(addsuffix .h,$(subst .,_,$(BINFILES)))

#---------------------------------------------------------------------------------
# build a list of include paths
#---------------------------------------------------------------------------------
export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD) \
			-I$(LIBOGC_INC)

#---------------------------------------------------------------------------------
# build a list of library paths
#---------------------------------------------------------------------------------
export LIBPATHS	:=	-L$(LIBOGC_LIB) $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export OUTPUT	:=	$(CURDIR)/$(TARGET)

PYTHON	?=	python

.PHONY: $(BUILD) clean run iso wii gamecube dist

#---------------------------------------------------------------------------------
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
# the other console, without having to remember the variable
#---------------------------------------------------------------------------------
wii:
	@$(MAKE) --no-print-directory PLATFORM=wii

gamecube:
	@$(MAKE) --no-print-directory PLATFORM=gamecube

#---------------------------------------------------------------------------------
# a Homebrew Channel app folder: copy dist/apps/ onto the root of an SD card
#---------------------------------------------------------------------------------
dist: wii
	@mkdir -p dist/apps/msw-gcn
	@cp $(CURDIR)/msw-wii.dol dist/apps/msw-gcn/boot.dol
	@cp $(CURDIR)/tools/wii/meta.xml dist/apps/msw-gcn/meta.xml
	@echo "staged dist/apps/msw-gcn -- copy the apps/ folder to your SD card"

#---------------------------------------------------------------------------------
# a bootable disc image: the .dol plus the apploader in tools/apploader
#---------------------------------------------------------------------------------
iso: $(BUILD)
	@test "$(PLATFORM)" = gamecube || \
		(echo "iso builds a GameCube disc image -- drop PLATFORM=wii" && false)
	@$(PYTHON) $(CURDIR)/tools/mkiso.py

#---------------------------------------------------------------------------------
# both platforms, whichever one happens to be selected
#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr build build_wii dist
	@rm -f msw-gcn.elf msw-gcn.dol msw-gcn.iso msw-wii.elf msw-wii.dol

#---------------------------------------------------------------------------------
else

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

$(OFILES_SOURCES) : $(HFILES)

#---------------------------------------------------------------------------------
# embed compressed worlds (.mworld) and the texture atlas (.tpl) as binary data.
# Override the stock bin2o (which relies on mktemp, unavailable/unwritable in
# this toolchain shell) with one that stages the .s file in the build dir.
#---------------------------------------------------------------------------------
define bin2o
	bin2s -a 32 -H `(echo $(<F) | tr . _)`.h $< > $(<F).s
	$(CC) -x assembler-with-cpp $(CPPFLAGS) $(ASFLAGS) -c $(<F).s -o $(<F).o
	rm -f $(<F).s
endef

%.mworld.o %_mworld.h : %.mworld
	@echo $(notdir $<)
	@$(bin2o)

%.tpl.o %_tpl.h : %.tpl
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

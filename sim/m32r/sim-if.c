/* Main simulator entry points specific to the M32R.
   Copyright (C) 1996, 1997, 1998 Free Software Foundation, Inc.
   Contributed by Cygnus Support.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

#include "sim-main.h"
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include "sim-options.h"
#include "libiberty.h"
#include "bfd.h"

static void free_state (SIM_DESC);
static void print_m32r_misc_cpu (SIM_CPU *cpu, int verbose);

/* Records simulator descriptor so utilities like m32r_dump_regs can be
   called from gdb.  */
SIM_DESC current_state;

/* Cover function of sim_state_free to free the cpu buffers as well.  */

static void
free_state (SIM_DESC sd)
{
  if (STATE_MODULES (sd) != NULL)
    sim_module_uninstall (sd);
  sim_cpu_free_all (sd);
  sim_state_free (sd);
}

/* Create an instance of the simulator.  */

SIM_DESC
sim_open (kind, callback, abfd, argv)
     SIM_OPEN_KIND kind;
     host_callback *callback;
     struct _bfd *abfd;
     char **argv;
{
  char c;
  SIM_DESC sd = sim_state_alloc (kind, callback);

  /* The cpu data is kept in a separately allocated chunk of memory.  */
  if (sim_cpu_alloc_all (sd, 1, cgen_cpu_max_extra_bytes ()) != SIM_RC_OK)
    {
      free_state (sd);
      return 0;
    }

#if 0 /* FIXME: pc is in mach-specific struct */
  /* FIXME: watchpoints code shouldn't need this */
  {
    SIM_CPU *current_cpu = STATE_CPU (sd, 0);
    STATE_WATCHPOINTS (sd)->pc = &(PC);
    STATE_WATCHPOINTS (sd)->sizeof_pc = sizeof (PC);
  }
#endif

  if (sim_pre_argv_init (sd, argv[0]) != SIM_RC_OK)
    {
      free_state (sd);
      return 0;
    }

#if 0 /* FIXME: 'twould be nice if we could do this */
  /* These options override any module options.
     Obviously ambiguity should be avoided, however the caller may wish to
     augment the meaning of an option.  */
  if (extra_options != NULL)
    sim_add_option_table (sd, extra_options);
#endif

  /* getopt will print the error message so we just have to exit if this fails.
     FIXME: Hmmm...  in the case of gdb we need getopt to call
     print_filtered.  */
  if (sim_parse_args (sd, argv) != SIM_RC_OK)
    {
      free_state (sd);
      return 0;
    }

  /* Allocate a handler for the control registers and other devices
     if no memory for that range has been allocated by the user.
     All are allocated in one chunk to keep things from being
     unnecessarily complicated.  */
  if (sim_core_read_buffer (sd, NULL, read_map, &c, M32R_DEVICE_ADDR, 1) == 0)
    sim_core_attach (sd, NULL,
		     0 /*level*/,
		     access_read_write,
		     0 /*space ???*/,
		     M32R_DEVICE_ADDR, M32R_DEVICE_LEN /*nr_bytes*/,
		     0 /*modulo*/,
		     &m32r_devices,
		     NULL /*buffer*/);

  /* Allocate core managed memory if none specified by user.
     Use address 4 here in case the user wanted address 0 unmapped.  */
  if (sim_core_read_buffer (sd, NULL, read_map, &c, 4, 1) == 0)
    sim_do_commandf (sd, "memory region 0,0x%lx", M32R_DEFAULT_MEM_SIZE);

  /* check for/establish the reference program image */
  if (sim_analyze_program (sd,
			   (STATE_PROG_ARGV (sd) != NULL
			    ? *STATE_PROG_ARGV (sd)
			    : NULL),
			   abfd) != SIM_RC_OK)
    {
      free_state (sd);
      return 0;
    }

  /* If both cpu model and state architecture are set, ensure they're
     compatible.  If only one is set, set the other.  If neither are set,
     use the default model.  STATE_ARCHITECTURE is the bfd_arch_info data
     for the selected "mach" (bfd terminology).  */
  {
    SIM_CPU *cpu = STATE_CPU (sd, 0);

    if (! STATE_ARCHITECTURE (sd)
	/* Only check cpu 0.  STATE_ARCHITECTURE is for that one only.  */
	&& ! CPU_MACH (cpu))
      {
	/* Set the default model.  */
	const MODEL *model = sim_model_lookup (WITH_DEFAULT_MODEL);
	sim_model_set (sd, NULL, model);
      }
    if (STATE_ARCHITECTURE (sd)
	&& CPU_MACH (cpu))
      {
	if (strcmp (STATE_ARCHITECTURE (sd)->printable_name,
		    MACH_NAME (CPU_MACH (cpu))) != 0)
	  {
	    sim_io_eprintf (sd, "invalid model `%s' for `%s'\n",
			    MODEL_NAME (CPU_MODEL (cpu)),
			    STATE_ARCHITECTURE (sd)->printable_name);
	    free_state (sd);
	    return 0;
	  }
      }
    else if (STATE_ARCHITECTURE (sd))
      {
	/* Use the default model for the selected machine.
	   The default model is the first one in the list.  */
	const MACH *mach = sim_mach_lookup (STATE_ARCHITECTURE (sd)->printable_name);
	sim_model_set (sd, NULL, MACH_MODELS (mach));
      }
    else
      {
        STATE_ARCHITECTURE (sd) = bfd_scan_arch (MACH_NAME (CPU_MACH (cpu)));
      }
  }

  /* Establish any remaining configuration options.  */
  if (sim_config (sd) != SIM_RC_OK)
    {
      free_state (sd);
      return 0;
    }

  if (sim_post_argv_init (sd) != SIM_RC_OK)
    {
      free_state (sd);
      return 0;
    }

  /* Initialize various cgen things not done by common framework.  */
  cgen_init (sd);

  /* Open a copy of the opcode table.  */
  STATE_OPCODE_TABLE (sd) = m32r_cgen_opcode_open (STATE_ARCHITECTURE (sd)->mach,
						   CGEN_ENDIAN_BIG);
  m32r_cgen_init_dis (STATE_OPCODE_TABLE (sd));

  {
    int c;

    for (c = 0; c < MAX_NR_PROCESSORS; ++c)
      {
	/* Only needed for profiling, but the structure member is small.  */
	memset (CPU_M32R_MISC_PROFILE (STATE_CPU (sd, c)), 0,
		sizeof (* CPU_M32R_MISC_PROFILE (STATE_CPU (sd, c))));
	/* Hook in callback for reporting these stats */
	PROFILE_INFO_CPU_CALLBACK (CPU_PROFILE_DATA (STATE_CPU (sd, c)))
	  = print_m32r_misc_cpu;
      }
  }

  /* Store in a global so things like sparc32_dump_regs can be invoked
     from the gdb command line.  */
  current_state = sd;

  return sd;
}

void
sim_close (sd, quitting)
     SIM_DESC sd;
     int quitting;
{
  m32r_cgen_opcode_close (STATE_OPCODE_TABLE (sd));
  sim_module_uninstall (sd);
}

SIM_RC
sim_create_inferior (sd, abfd, argv, envp)
     SIM_DESC sd;
     struct _bfd *abfd;
     char **argv;
     char **envp;
{
  SIM_CPU *current_cpu = STATE_CPU (sd, 0);
  SIM_ADDR addr;

  if (abfd != NULL)
    addr = bfd_get_start_address (abfd);
  else
    addr = 0;
  sim_pc_set (current_cpu, addr);

#if 0
  STATE_ARGV (sd) = sim_copy_argv (argv);
  STATE_ENVP (sd) = sim_copy_argv (envp);
#endif

  return SIM_RC_OK;
}

/* PROFILE_CPU_CALLBACK */

static void
print_m32r_misc_cpu (SIM_CPU *cpu, int verbose)
{
  SIM_DESC sd = CPU_STATE (cpu);
  char buf[20];

  if (CPU_PROFILE_FLAGS (cpu) [PROFILE_INSN_IDX])
    {
      sim_io_printf (sd, "Miscellaneous Statistics\n\n");
      sim_io_printf (sd, "  %-*s %s\n\n",
		     PROFILE_LABEL_WIDTH, "Fill nops:",
		     sim_add_commas (buf, sizeof (buf),
				     CPU_M32R_MISC_PROFILE (cpu)->fillnop_count));
      if (STATE_ARCHITECTURE (sd)->mach == bfd_mach_m32rx)
	sim_io_printf (sd, "  %-*s %s\n\n",
		       PROFILE_LABEL_WIDTH, "Parallel insns:",
		       sim_add_commas (buf, sizeof (buf),
				       CPU_M32R_MISC_PROFILE (cpu)->parallel_count));
    }
}

void
sim_do_command (sd, cmd)
     SIM_DESC sd;
     char *cmd;
{ 
  char **argv;

  if (cmd == NULL)
    return;

  argv = buildargv (cmd);

  if (argv[0] != NULL
      && strcasecmp (argv[0], "info") == 0
      && argv[1] != NULL
      && strncasecmp (argv[1], "reg", 3) == 0)
    {
      SI val;

      /* We only support printing bbpsw,bbpc here as there is no equivalent
	 functionality in gdb.  */
      if (argv[2] == NULL)
	sim_io_eprintf (sd, "Missing register in `%s'\n", cmd);
      else if (argv[3] != NULL)
	sim_io_eprintf (sd, "Too many arguments in `%s'\n", cmd);
      else if (strcasecmp (argv[2], "bbpsw") == 0)
	{
	  val = a_m32r_h_cr_get (STATE_CPU (sd, 0), H_CR_BBPSW);
	  sim_io_printf (sd, "bbpsw 0x%x %d\n", val, val);
	}
      else if (strcasecmp (argv[2], "bbpc") == 0)
	{
	  val = a_m32r_h_cr_get (STATE_CPU (sd, 0), H_CR_BBPC);
	  sim_io_printf (sd, "bbpc 0x%x %d\n", val, val);
	}
      else
	sim_io_eprintf (sd, "Printing of register `%s' not supported with `sim info'\n",
			argv[2]);
    }
  else
    {
      if (sim_args_command (sd, cmd) != SIM_RC_OK)
	sim_io_eprintf (sd, "Unknown sim command `%s'\n", cmd);
    }

  freeargv (argv);
}

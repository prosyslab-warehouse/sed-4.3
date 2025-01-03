/*  GNU SED, a batch stream editor.
    Copyright (C) 1989-2016 Free Software Foundation, Inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3, or (at your option)
    any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA. */


#include "sed.h"


#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include "getopt.h"
#include "progname.h"
#include "version.h"

#include "version-etc.h"

#define AUTHORS \
   _("Jay Fenlason"), \
   _("Tom Lord"), \
   _("Ken Pizzini"), \
   _("Paolo Bonzini")

int extended_regexp_flags = 0;

/* one-byte buffer delimiter */
char buffer_delimiter = '\n';

/* If set, fflush(stdout) on every line output. */
bool unbuffered = false;

/* If set, don't write out the line unless explicitly told to */
bool no_default_output = false;

/* If set, reset line counts on every new file. */
bool separate_files = false;

/* If set, follow symlinks when processing in place */
bool follow_symlinks = false;

/* If set, opearate in 'sandbox' mode */
bool sandbox = false;

/* How do we edit files in-place? (we don't if NULL) */
char *in_place_extension = NULL;

/* The mode to use to read/write files, either "r"/"w" or "rb"/"wb".  */
char const *read_mode = "r";
char const *write_mode = "w";

/* Do we need to be pedantically POSIX compliant? */
enum posixicity_types posixicity;

/* How long should the `l' command's output line be? */
countT lcmd_out_line_len = 70;

/* The complete compiled SED program that we are going to run: */
static struct vector *the_program = NULL;

/* When we've created a temporary for an in-place update,
   we may have to exit before the rename.  This is the name
   of the temporary that we'll have to unlink via an atexit-
   registered cleanup function.  */
static char const *G_file_to_unlink;

struct localeinfo localeinfo;

/* When exiting between temporary file creation and the rename
   associated with a sed -i invocation, remove that file.  */
static void
cleanup (void)
{
  if (G_file_to_unlink)
    unlink (G_file_to_unlink);
}

/* Note that FILE must be removed upon exit.  */
void
register_cleanup_file (char const *file)
{
  G_file_to_unlink = file;
}

/* Clear the global file-to-unlink global.  */
void
cancel_cleanup (void)
{
  G_file_to_unlink = NULL;
}

static void usage (int);
static void
contact(int errmsg)
{
  FILE *out = errmsg ? stderr : stdout;
#ifndef REG_PERL
  fprintf(out, _("GNU sed home page: <http://www.gnu.org/software/sed/>.\n\
General help using GNU software: <http://www.gnu.org/gethelp/>.\n"));
#endif

  /* Only print the bug report address for `sed --help', otherwise we'll
     get reports for other people's bugs.  */
  if (!errmsg)
    fprintf(out, _("E-mail bug reports to: <%s>.\n"), PACKAGE_BUGREPORT);
}

_Noreturn static void
usage(int status)
{
  FILE *out = status ? stderr : stdout;

#ifdef REG_PERL
#define PERL_HELP _("  -R, --regexp-perl" \
                    "\n                 use Perl 5's regular expressions" \
                    " syntax in the script.\n")
#endif

  fprintf(out, _("\
Usage: %s [OPTION]... {script-only-if-no-other-script} [input-file]...\n\
\n"), myname);

  fprintf(out, _("  -n, --quiet, --silent\n\
                 suppress automatic printing of pattern space\n"));
  fprintf(out, _("  -e script, --expression=script\n\
                 add the script to the commands to be executed\n"));
  fprintf(out, _("  -f script-file, --file=script-file\n\
                 add the contents of script-file to the commands" \
                 " to be executed\n"));
#ifdef ENABLE_FOLLOW_SYMLINKS
  fprintf(out, _("  --follow-symlinks\n\
                 follow symlinks when processing in place\n"));
#endif
  fprintf(out, _("  -i[SUFFIX], --in-place[=SUFFIX]\n\
                 edit files in place (makes backup if SUFFIX supplied)\n"));
#if defined WIN32 || defined _WIN32 || defined __CYGWIN__ \
  || defined MSDOS || defined __EMX__
  fprintf(out, _("  -b, --binary\n\
                 open files in binary mode (CR+LFs are not" \
                 " processed specially)\n"));
#endif
  fprintf(out, _("  -l N, --line-length=N\n\
                 specify the desired line-wrap length for the `l' command\n"));
  fprintf(out, _("  --posix\n\
                 disable all GNU extensions.\n"));
  fprintf(out, _("  -E, -r, --regexp-extended\n\
                 use extended regular expressions in the script\n\
                 (for portability use POSIX -E).\n"));
#ifdef REG_PERL
  fprintf(out, PERL_HELP);
#endif
  fprintf(out, _("  -s, --separate\n\
                 consider files as separate rather than as a single,\n\
                 continuous long stream.\n"));
  fprintf(out, _("      --sandbox\n\
                 operate in sandbox mode.\n"));
  fprintf(out, _("  -u, --unbuffered\n\
                 load minimal amounts of data from the input files and flush\n\
                 the output buffers more often\n"));
  fprintf(out, _("  -z, --null-data\n\
                 separate lines by NUL characters\n"));
  fprintf(out, _("      --help     display this help and exit\n"));
  fprintf(out, _("      --version  output version information and exit\n"));
  fprintf(out, _("\n\
If no -e, --expression, -f, or --file option is given, then the first\n\
non-option argument is taken as the sed script to interpret.  All\n\
remaining arguments are names of input files; if no input files are\n\
specified, then the standard input is read.\n\
\n"));
  contact (status);

  ck_fclose (NULL);
  exit (status);
}

int
main (int argc, char **argv)
{
#ifdef REG_PERL
#define SHORTOPTS "bsnrzRuEe:f:l:i::V:"
#else
#define SHORTOPTS "bsnrzuEe:f:l:i::V:"
#endif

  enum { SANDBOX_OPTION = CHAR_MAX+1 };

  static const struct option longopts[] = {
    {"binary", 0, NULL, 'b'},
    {"regexp-extended", 0, NULL, 'r'},
#ifdef REG_PERL
    {"regexp-perl", 0, NULL, 'R'},
#endif
    {"expression", 1, NULL, 'e'},
    {"file", 1, NULL, 'f'},
    {"in-place", 2, NULL, 'i'},
    {"line-length", 1, NULL, 'l'},
    {"null-data", 0, NULL, 'z'},
    {"zero-terminated", 0, NULL, 'z'},
    {"quiet", 0, NULL, 'n'},
    {"posix", 0, NULL, 'p'},
    {"silent", 0, NULL, 'n'},
    {"sandbox", 0, NULL, SANDBOX_OPTION},
    {"separate", 0, NULL, 's'},
    {"unbuffered", 0, NULL, 'u'},
    {"version", 0, NULL, 'v'},
    {"help", 0, NULL, 'h'},
#ifdef ENABLE_FOLLOW_SYMLINKS
    {"follow-symlinks", 0, NULL, 'F'},
#endif
    {NULL, 0, NULL, 0}
  };

  int opt;
  int return_code;
  const char *cols = getenv("COLS");

  program_name = argv[0];
  initialize_main (&argc, &argv);
#if HAVE_SETLOCALE
  /* Set locale according to user's wishes.  */
  setlocale (LC_ALL, "");
#endif
  set_program_name (argv[0]);
  initialize_mbcs ();
  init_localeinfo (&localeinfo);

  /* Arrange to remove any un-renamed temporary file,
     upon premature exit.  */
  atexit (cleanup);

#if ENABLE_NLS

  /* Tell program which translations to use and where to find.  */
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);
#endif

  if (getenv("POSIXLY_CORRECT") != NULL)
    posixicity = POSIXLY_CORRECT;
  else
    posixicity = POSIXLY_EXTENDED;

  /* If environment variable `COLS' is set, use its value for
     the baseline setting of `lcmd_out_line_len'.  The "-1"
     is to avoid gratuitous auto-line-wrap on ttys.
   */
  if (cols)
    {
      countT t = atoi(cols);
      if (t > 1)
        lcmd_out_line_len = t-1;
    }

  myname = *argv;
  while ((opt = getopt_long(argc, argv, SHORTOPTS, longopts, NULL)) != EOF)
    {
      switch (opt)
        {
        case 'n':
          no_default_output = true;
          break;
        case 'e':
          the_program = compile_string(the_program, optarg, strlen(optarg));
          break;
        case 'f':
          the_program = compile_file(the_program, optarg);
          break;

        case 'z':
          buffer_delimiter = 0;
          break;

        case 'F':
          follow_symlinks = true;
          break;

        case 'i':
          separate_files = true;
          if (optarg == NULL)
            /* use no backups */
            in_place_extension = ck_strdup ("*");

          else if (strchr(optarg, '*') != NULL)
            in_place_extension = ck_strdup(optarg);

          else
            {
              in_place_extension = MALLOC (strlen(optarg) + 2, char);
              in_place_extension[0] = '*';
              strcpy (in_place_extension + 1, optarg);
            }

          break;

        case 'l':
          lcmd_out_line_len = atoi(optarg);
          break;

        case 'p':
          posixicity = POSIXLY_BASIC;
          break;

        case 'b':
          read_mode = "rb";
          write_mode = "wb";
          break;

        case 'E':
        case 'r':
#ifdef REG_PERL
          if (extended_regexp_flags && (extended_regexp_flags!=REG_EXTENDED))
            usage(EXIT_BAD_USAGE);
#endif
          extended_regexp_flags = REG_EXTENDED;
          break;

#ifdef REG_PERL
        case 'R':
          if (extended_regexp_flags && (extended_regexp_flags!=REG_PERL)))
            usage(EXIT_BAD_USAGE);
          extended_regexp_flags = REG_PERL;
          break;
#endif

        case 's':
          separate_files = true;
          break;

        case SANDBOX_OPTION:
          sandbox = true;
          break;

        case 'u':
          unbuffered = true;
          break;

        case 'v':
          version_etc(stdout, program_name, PACKAGE_NAME, Version,
                      AUTHORS, (char *) NULL);
          contact(false);
          ck_fclose (NULL);
          exit (EXIT_SUCCESS);
        case 'h':
          usage(EXIT_SUCCESS);
        default:
          usage(EXIT_BAD_USAGE);
        }
    }

  if (!the_program)
    {
      if (optind < argc)
        {
          char *arg = argv[optind++];
          the_program = compile_string(the_program, arg, strlen(arg));
        }
      else
        usage(EXIT_BAD_USAGE);
    }
  check_final_program(the_program);

  return_code = process_files(the_program, argv+optind);

  finish_program();
  ck_fclose(NULL);

  return return_code;
}

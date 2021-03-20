#!/usr/bin/env python
#
# Copyright (c) 2019 the Massachusetts Institute of Technology
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#
#
# This script coverts the output of ltrace on a given program
# invocation into a trace file of memory allocations and frees.

from __future__ import print_function
import argparse
import logging
import re
import subprocess
import sys

can_plot = True
try:
  import matplotlib
  matplotlib.use('PDF')
  import matplotlib.pyplot as plt
except ImportError:
  can_plot = False

# Convert the output of ltrace into a trace file.
def make_trace_from_ltrace(ltrace_out, clean):
  logger = logging.getLogger(sys.argv[0])
  logger.info(ltrace_out)

  # Data structures to track allocation IDs, based on library calls
  # and addresses in ltrace.

  # Next allocation ID
  next_id = 0
  # Dictionary mapping addresses to allocation IDs
  addresses = {}
  # Output lines for the final trace
  trace_lines = []

  # Data structures for generating visualization of trace.
  # Dictionary mapping allocation IDs to sizes
  sizes = {}
  current_heap_in_use = 0
  max_heap_in_use = 0
  heap_in_use_points = []

  # Process the ltrace output line by line.
  for line in ltrace_out.split('\n'):
    # Parse the library calls in the ltrace
    if "malloc" in line:
      # Parse the malloc call
      malloc_parse = re.search("malloc\((\d+)\)\s*=\s*(0x[0-9a-fA-F]+)", line)
      if malloc_parse is None:
        logger.error("Skipping " + line)
        continue

      # The address returned by malloc
      addr = int(malloc_parse.group(2), 16)
      # The size passed to malloc
      size = int(malloc_parse.group(1))
      # Get an allocation ID for this malloc call
      alloc_id = next_id
      if addr in addresses:
        logger.warning("malloc returned an address already in the dictionary: " + line)
      else:
        # Record the new allocation ID
        addresses[addr] = next_id
        next_id += 1
        trace_lines.append("a " + str(alloc_id) + " " + str(size))

        # Update heap-in-use data
        sizes[alloc_id] = size
        current_heap_in_use += size
        if current_heap_in_use > max_heap_in_use:
          max_heap_in_use = current_heap_in_use
        heap_in_use_points.append(current_heap_in_use)

    elif "free" in line:
      # Parse the free call
      free_parse = re.search("free\(((0x)?[0-9a-fA-F]+)\)", line)
      if free_parse is None:
        logger.error("Skipping " + line)
        continue

      # The free'd address
      addr = int(free_parse.group(1), 16)
      if addr in addresses:
        # Record the free of the existing allocation ID
        alloc_id = addresses[addr]
        trace_lines.append("f " + str(alloc_id))

        # Update heap-in-use data
        current_heap_in_use -= sizes[alloc_id]
        heap_in_use_points.append(current_heap_in_use)
        del sizes[alloc_id]
        # Remove the free'd address from the dictionary
        del addresses[addr]

      else:
        # TODO: This case is typically hit by a call to free(NULL),
        # but trace files have no way to perform free(NULL).  Find a
        # way to extend trace files to support this case.
        #
        # This case can also be reached if a library call, such as
        # realpath, invokes malloc() internally and expects the callee
        # to call free().
        logger.warning("Skipping " + line)

    elif "realloc" in line:
      # Parse the call to realloc
      realloc_parse = re.search("realloc\(((0x)?[0-9a-fA-F]+),\s*(\d+)\)\s*=\s*(0x[0-9a-fA-F]+)", line)
      if realloc_parse is None:
        logger.error("Skipping " + line)
        continue

      # The old address passed to realloc.  This should be 0 (i.e.,
      # NULL) or an address of a previously allocated block of memory.
      oldaddr = int(realloc_parse.group(1), 16)
      # The new address, returned by realloc.
      addr = int(realloc_parse.group(4), 16)
      # The new size.
      size = int(realloc_parse.group(3))
      if oldaddr == 0:
        # realloc(NULL, size): Act like malloc(size)
        # Record a new allocation ID
        alloc_id = next_id
        addresses[addr] = next_id
        next_id += 1
        trace_lines.append("r " + str(alloc_id) + " " + str(size))

        # Update heap-in-use data
        sizes[alloc_id] = size
        current_heap_in_use += size
        if current_heap_in_use > max_heap_in_use:
          max_heap_in_use = current_heap_in_use
        heap_in_use_points.append(current_heap_in_use)

      elif oldaddr in addresses:
        # Record a realloc of an existing allocation ID
        alloc_id = addresses[oldaddr]
        trace_lines.append("r " + str(alloc_id) + " " + str(size))
        # If realloc returned a new address, associate the allocation
        # ID with the new address, and remove the old address from the
        # dictionary.
        if addr != oldaddr:
          addresses[addr] = addresses[oldaddr]
          del addresses[oldaddr]

        # Update heap-in-use data
        current_heap_in_use += size - sizes[alloc_id]
        if current_heap_in_use > max_heap_in_use:
          max_heap_in_use = current_heap_in_use
        sizes[alloc_id] = size
        heap_in_use_points.append(current_heap_in_use)

      else:
        logger.warning("Skipping " + line)

    elif "calloc" in line:
      # Parse a call to calloc
      calloc_parse = re.search("calloc\((\d+),\s*(\d+)\)\s*=\s*(0x[0-9a-fA-F]+)", line)
      if calloc_parse is None:
        logger.error("Skipping " + line)
        continue

      # The allocated address, returned by calloc.
      addr = int(calloc_parse.group(3), 16)
      # The total size of the allocation, which is the product of the
      # arguments to calloc.
      size = int(calloc_parse.group(1)) * int(calloc_parse.group(2))
      # Get the allocation ID for this call.
      alloc_id = next_id
      if addr in addresses:
        logger.warning("calloc returned an address already in the dictionary: " + line)
      else:
        # Record the new allocation ID
        addresses[addr] = next_id
        next_id += 1
        trace_lines.append("a " + str(alloc_id) + " " + str(size))

        # Update heap-in-use data
        sizes[alloc_id] = size
        current_heap_in_use += size
        if current_heap_in_use > max_heap_in_use:
          max_heap_in_use = current_heap_in_use
        heap_in_use_points.append(current_heap_in_use)

    # TODO: Add support for rarer library calls that internally call
    # malloc, e.g., realpath.
    else:
      logger.warning("Skipping " + line)

  if clean:
    logger.info("Freeing " + str(len(addresses)) + " outstanding allocations")
    # Free any outstanding allocations
    for addr in addresses:
      alloc_id = addresses[addr]
      # Add the free to the trace
      trace_lines.append("f " + str(alloc_id))
      # Update heap-in-use data
      current_heap_in_use -= sizes[alloc_id]
      heap_in_use_points.append(current_heap_in_use)
  else:
    logger.info("Trace ended with " + str(len(addresses)) + " outstanding allocations, " +
                "totalling " + str(current_heap_in_use) + " bytes not free'd.")

  # Add header information to trace output:
  # 1) Suggested heap size (unused)
  # 2) Number of IDs
  # 3) Numer of ops
  # 4) Weight (unused)
  trace = [str(max_heap_in_use), str(next_id), str(len(trace_lines)), "1"] + trace_lines
  return trace, heap_in_use_points

def main():
  # Script arguments
  arg_parser = argparse.ArgumentParser()
  arg_parser.add_argument('--output', '-o', default=sys.stdout,
                          type=argparse.FileType('w'),
                          help='output filename for trace')
  arg_parser.add_argument('--show', '-s', action='store_true',
                          default=False,
                          help='plot a visualization of the trace')
  arg_parser.add_argument('--no-clean', '-nc', action='store_true',
                          default=False,
                          help='don\'t free outstanding allocations at the end of the trace')
  arg_parser.add_argument('--verbose', '-v', action='count',
                          default=0)
  arg_parser.add_argument('command', nargs='+', help='program command to trace')
  
  args = arg_parser.parse_args()

  # Set the logging level
  if args.verbose == 1:
    logging.basicConfig(level=logging.WARNING)
  elif args.verbose == 2:
    logging.basicConfig(level=logging.INFO)
  else:
    logging.basicConfig(level=logging.ERROR)

  logger = logging.getLogger(sys.argv[0])

  # Get the program command to run
  prog = args.command
  # Construct ltrace invocation
  cmd = ["ltrace", "-f", "-e", "free+*alloc", "--"] + prog

  # Grab the output of ltrace
  p = subprocess.Popen(cmd, stderr=subprocess.PIPE, universal_newlines=True)
  (ltrace_stdout, ltrace_stderr) = p.communicate()

  if p.returncode != 0:
    logger.error("Command exited with return code " + str(p.returncode))
    logger.error("  " + ' '.join(cmd))
    if ltrace_stdout is not None:
      logger.error("stdout: " + ltrace_stdout)
    if ltrace_stderr is not None:
      logger.error("stderr: " + ltrace_stderr)
    exit()

  # Construct a trace from the ltrace output
  (trace, heap_in_use_points) = make_trace_from_ltrace(ltrace_stderr,
                                                     not args.no_clean)

  # Print the trace
  print('\n'.join(trace), file=args.output)

  if args.show:
    if can_plot:
      # Print the trace visualization
      plt.step(y=heap_in_use_points, x=range(1, len(heap_in_use_points)+1),
               where='post')
      plt.ylabel('Heap in use (B)')
      plt.xlabel('Op')
      if args.output != sys.stdout:
        plt.savefig(args.output.name + "-vis.pdf", bbox_inches='tight')
      else:
        plt.savefig("trace-vis.pdf", bbox_inches='tight')
    else:
      logger.error("matplotlib required to generate trace visualization.")

if __name__ == '__main__':
  main()

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
import os
import re
import sys

can_plot = True
try:
  import matplotlib
  matplotlib.use('PDF')
  import matplotlib.pyplot as plt
except ImportError:
  can_plot = False

# Convert the output of ltrace into a trace file.
def analyze_heap_in_use(trace_file):
  logger = logging.getLogger(sys.argv[0])
  logger.info(trace_file)

  # Data structures to track allocation IDs, based on library calls
  # and addresses in ltrace.

  # Data structures for generating visualization of trace.
  # Dictionary mapping allocation IDs to sizes
  sizes = {}
  current_heap_in_use = 0
  max_heap_in_use = 0
  heap_in_use_points = []

  # Process the trace file line by line.
  for line in trace_file:
    # Parse each line of the trace file
    if "a" in line:
      # Parse the allocation
      alloc_parse = re.search("a\s+(\d+)\s+(\d+)", line)
      if alloc_parse is None:
        logger.error("Skipping " + line)
        continue

      # Allocation ID
      alloc_id = int(alloc_parse.group(1))
      # Size
      size = int(alloc_parse.group(2))

      # Verify that no size is recorded for this allocation
      if alloc_id in sizes:
        logger.error("Allocation " + str(alloc_id) + " has already been allocated with size " + str(size))
        continue

      # Record the size of the allocation
      sizes[alloc_id] = size
      # Update the heap in use
      current_heap_in_use += size
      if current_heap_in_use > max_heap_in_use:
        max_heap_in_use = current_heap_in_use
      heap_in_use_points.append(current_heap_in_use)

    elif "f" in line:
      # Parse the free
      free_parse = re.search("f\s+(\d+)", line)
      if free_parse is None:
        logger.error("Skipping " + line)
        continue

      # Allocation ID
      alloc_id = int(free_parse.group(1))

      # Verify that a size is recorded for this allocation
      if alloc_id not in sizes:
        logger.error("No size known for allocation " + str(alloc_id))
        continue

      current_heap_in_use -= sizes[alloc_id]
      heap_in_use_points.append(current_heap_in_use)
      del sizes[alloc_id]

    elif "r" in line:
      # Parse the realloc
      realloc_parse = re.search("r\s+(\d+)\s+(\d+)", line)
      if realloc_parse is None:
        logger.error("Skipping " + line)
        continue

      # Allocation ID
      alloc_id = int(realloc_parse.group(1))
      # New size
      size = int(realloc_parse.group(2))

      # Update current heap in use
      if alloc_id in sizes:
        current_heap_in_use += size - sizes[alloc_id]
      else:
        current_heap_in_use += size

      # Update the maximum heap in use
      if current_heap_in_use > max_heap_in_use:
        max_heap_in_use = current_heap_in_use
      # Record next point in plot
      heap_in_use_points.append(current_heap_in_use)
      # Record new size
      sizes[alloc_id] = size

    else:
      logger.warning("Skipping " + line)

  return heap_in_use_points

def main():
  # Script arguments
  arg_parser = argparse.ArgumentParser()
  arg_parser.add_argument('--trace-file', '-f', default=sys.stdin,
                          type=argparse.FileType('r'),
                          help='trace file to plot')
  arg_parser.add_argument('--allocator-usage', '-a', default=None,
                          type=argparse.FileType('r'),
                          help='allocator usage to plot')
  arg_parser.add_argument('--verbose', '-v', action='count',
                          default=0)

  if not can_plot:
    logger.error("matplotlib required to generate trace visualization.")
    exit()

  args = arg_parser.parse_args()

  # Set the logging level
  if args.verbose == 1:
    logging.basicConfig(level=logging.WARNING)
  elif args.verbose == 2:
    logging.basicConfig(level=logging.INFO)
  else:
    logging.basicConfig(level=logging.ERROR)

  logger = logging.getLogger(sys.argv[0])

  # Process the trace file to get the heap in use at each step
  heap_in_use = analyze_heap_in_use(args.trace_file)

  # Print the trace visualization
  plt.step(y=heap_in_use, x=range(1, len(heap_in_use)+1),
           where='post', label='Heap in use')
  if args.allocator_usage is not None:
    alloc_use = args.allocator_usage.read().split()
    if len(alloc_use) != len(heap_in_use):
      logger.error("Allocator-usage data does not have the same number of ops as trace file")
      logger.info(alloc_use)
    else:
      plt.step(y=alloc_use, x=range(1, len(heap_in_use)+1),
               where='post', label='Allocated')
  plt.ylabel('Memory (B)')
  plt.xlabel('Op')
  plt.legend(loc='best')
  if args.trace_file != sys.stdin:
    plt.savefig(os.path.basename(args.trace_file.name) + "-vis.pdf", bbox_inches='tight')
  else:
    plt.savefig("trace-vis.pdf", bbox_inches='tight')

if __name__ == '__main__':
  main()


#pragma once

// Entry point for the simulation worker process:
//   linspice --sim-worker <netlist.cir> <output.raw> [--live <live.bin>]
//
// The worker loads libngspice with dlopen(), runs the circuit in a background
// thread and reports on stdout, one message per line:
//   LOG <text>          simulator console output
//   STAT <text>         progress/status ("tran: 45.2%")
//   PROGRESS <percent>
//   PLOT <name>         a plot was written to the output raw file
//   DONE <exitcode>
// It accepts "halt" on stdin to stop early (partial results are still written).
int runSimWorker(int argc, char **argv);

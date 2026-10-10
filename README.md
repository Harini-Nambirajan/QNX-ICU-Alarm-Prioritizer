# QNX ICU Alarm Prioritizer

A C-based embedded systems mini project that demonstrates multi-patient ICU alarm classification and prioritization using POSIX threads, queues, dynamic priority scoring, and hardware alarm indicators.

## Overview

The QNX ICU Alarm Prioritizer simulates multiple patients generating vital-sign readings. The program classifies readings by severity, filters repeated alarms, and selects alarms for dispatch according to priority.

The project also includes a web dashboard interface for displaying patient-monitoring information.

## Features

- Simulates vital-sign data for 10 patients.
- Classifies alarms as Normal, Warning, Risk/Severe, or Critical.
- Uses POSIX threads for concurrent processing.
- Uses a thread-safe FIFO input queue and a priority queue.
- Suppresses repeated alarms within a configured time window.
- Applies priority aging and deadline escalation.
- Checks local treatment capability and flags transfer requirements.
- Supports terminal alarm indicators and optional GPIO output on compatible hardware.
- Includes a web dashboard interface.

## Project Structure

```text
QNX-ICU-Alarm-Prioritizer/
├── ICU_Alarm_Prioritizer.c
├── qnx_icu_patient_monitoring_dashboard.html
├── README.md
```

The `assets` folder should contain the CSS, JavaScript, images, and other files required by the dashboard.

## Technologies Used

- C
- POSIX Threads (pthreads)
- QNX Momentics IDE
- HTML, CSS, and JavaScript
- Raspberry Pi 4 GPIO support

## Setup and Execution

1. Clone or download this repository.
2. Open `ICU_Alarm_Prioritizer.c` in QNX Momentics.
3. Configure the project for your target QNX environment.
4. Build and run the program using the appropriate target or simulator.
5. Open `qnx_icu_patient_monitoring_dashboard.html` in a browser to view the dashboard interface.

The dashboard's live connection to the C application requires a communication component if one is not already included.

## Hardware Notes

GPIO output requires compatible hardware, suitable permissions, and correct platform-specific GPIO configuration. The simulation path may run without physical GPIO access.

## Limitations and Safety

This is an educational prototype intended to demonstrate embedded and real-time system concepts. It is not a clinically validated medical device and must not be used to make real patient-care decisions.

Alarm thresholds, scheduling behavior, transfer logic, and hardware support require further testing and validation before any real-world deployment.

## Future Enhancements

- Integrate the C application with the dashboard using a defined communication interface.
- Add real sensor input and validated data handling.
- Improve event logging and performance reporting.
- Test scheduling and timing behavior on the target QNX hardware.

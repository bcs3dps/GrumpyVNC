/*--------------------------------------------------------------------------*/
/* Copyright (c) 2026 B. C. Services                                         */
/* Part of this GrumpyScreen tree and under its licence: GPL-3.0, in        */
/* LICENSE at the top of the tree.                                          */
/*--------------------------------------------------------------------------*/
/* main_options.h:                                                          */
/*                                                                          */
/* The startup settings main() decides -- from the command line, else the   */
/* configuration file, else a default -- that other parts of the program   */
/* need to ask about once start-up is over.  main.cpp owns the answers:     */
/* the command line is only visible there, and the command line wins over  */
/* the configuration, so reading the configuration file directly elsewhere */
/* would give the wrong answer whenever an option overrode it.              */
/*                                                                          */
/* Ask only after main() has passed its option handling (every panel is     */
/* built after that, inside GuppyScreen::init); before it, each answer is   */
/* its default.                                                             */
/*--------------------------------------------------------------------------*/

#ifndef MAIN_OPTIONS_H
#define MAIN_OPTIONS_H

/*--------------------------------------------------------------------------*/
/* MAIN_VncWifiWanted:                                                      */
/*                                                                          */
/* Whether the WiFi panel is wanted in VNC mode: --vnc-wifi on|off, else     */
/* the [vnc] section's "wifi", else off.  Served over VNC -- typically from  */
/* a Raspberry Pi with no panel of its own -- the WiFi panel would manage    */
/* the WiFi of the machine GrumpyScreen runs on, and a mis-tap there could  */
/* take that machine, and the VNC connection with it, off the network; so   */
/* it is left out unless this says otherwise.  On a local panel the WiFi    */
/* panel is always built, as upstream builds it, and this is not consulted. */
/*                                                                          */
/* Arguments:                                                               */
/*     None.                                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     bool : true when VNC mode should build the WiFi panel and its tab;   */
/*            false (the default) to leave both out.                         */
/*--------------------------------------------------------------------------*/

bool MAIN_VncWifiWanted(void);

#endif // MAIN_OPTIONS_H

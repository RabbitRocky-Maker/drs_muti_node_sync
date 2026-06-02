// -----------------------------------------------------------------------------
// THESIS TEMPLATE FOR UAS TECHNIKUM WIEN
// Author: M. Horauer
// GITHUB: https://github.com/mhorauer/
// LICENSE: GPL-3.0-or-later
// -----------------------------------------------------------------------------

#import "template/src/uastw-thesis-lib.typ": *

// -----------------------------------------------------------------------------
// ---[ ToDo ]------------------------------------------------------------------
// Adjust the variables below ...

#let lan = "en"
#let std = "Distributed and Embedded Systems" // Clear study program for lab report

#let title = [Lab Report of Exercise 2:\ Multi-Node Clock Sync]
#let subTitle = "Distributed User-Space Synchronization with < 100 µs Precision"

#let authorName  = "Simon Krist"
#let authorID   = ""
#let authorName2 = "Zhi Heng Yao"
#let authorID2  = ""

#let adv1 = ""
#let adv2 = ""
#let loc = "Wien"

// -----------------------------------------------------------------------------
// ---[ DO NOT TOUCH ]----------------------------------------------------------
#let thesisType = "LAB REPORT"

// --- OUTPUT THE TITLEPAGE ----------------------------------------------------
#set page(numbering: none)
#show: uastw-thesis-titlepage.with(
	language: lan, 
    thesis-type: thesisType, 
    degree: "", 
    study-program: std, 
    thesis-title: title, 
    thesis-subtitle: subTitle,
    author: authorName,
    authorid: authorID,
    author2: authorName2,
    authorid2: authorID2,
    advisor1: adv1,
    advisor2: adv2,    
    location: loc)

// --- SETUP THE PAGE STYLING & SOME VARIABLES ---------------------------------
#show: uastw-thesis-page-setup
#show "LaTeX": latex 
#show "BibTeX": bibtex 
#show "Rust": rust

// --- OUTPUT THE PAGE OF DECLARATION ------------------------------------------
// Removed for Lab Report

// --- WE START WITH PAGE NUMBERING @SUMMARY ----------------------------------
#set page(footer: context [
	#set text(twgray, size: 10pt) 
	#align(right, counter(page).display("1"))
	])
#set page(numbering: "1")

// --- INSERT SUMMARY ----------------------------------------------------------
#include "sections/01_summary.typ"

// --- INSERT TABLE OF CONTENTS ------------------------------------------------
#outline(
	title: if lan == "en" [Table of Contents] else [Inhaltsverzeichnis],
    indent: auto,
)

// Just in case - reset the counter for Headings ...
//
#counter(heading).update(0)
// =============================================================================
// --[ ADJUST YOUR CONTENT FILES BY ADDING/MODIFYING SECTIONS ]-----------------
//
#include "sections/10_requirements.typ"
#include "sections/20_architecture.typ"
#include "sections/30_implementation.typ"
#include "sections/40_testing.typ"
#include "sections/50_results.typ"

// --- APPENDICES --------------------------------------------------------------
#v(2em)
#include "sections/91_lists.typ"
#include "sections/92_abbreviations.typ"
#include "sections/93_aitools.typ"

// --- BIBLIOGRAPHY ------------------------------------------------------------
#v(2em)
#bibliography("sections/90_works.bib", style: "ieee")

// -----------------------------------------------------------------------------
// EOF

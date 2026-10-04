#!/usr/bin/env python3
"""Render the M10 machine report as a compact, printable PDF."""
import argparse
import json
from pathlib import Path

from reportlab.lib import colors
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.units import inch
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle, PageBreak


def render(report, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    styles = getSampleStyleSheet()
    styles.add(ParagraphStyle(name="HeroM10", parent=styles["Title"], fontSize=19, leading=23,
                              textColor=colors.HexColor("#103568"), spaceAfter=12))
    styles.add(ParagraphStyle(name="SectionM10", parent=styles["Heading2"], fontSize=11,
                              textColor=colors.HexColor("#103568"), spaceBefore=14, spaceAfter=7))
    styles.add(ParagraphStyle(name="BodyM10", parent=styles["BodyText"], fontSize=9, leading=13,
                              spaceAfter=7))
    styles.add(ParagraphStyle(name="NoteM10", parent=styles["BodyText"], fontSize=8, leading=11,
                              textColor=colors.HexColor("#495669"), spaceAfter=6))
    styles.add(ParagraphStyle(name="CellM10", parent=styles["Normal"], fontSize=8, leading=10))
    p = lambda content, style="BodyM10": Paragraph(content, styles[style])
    doc = SimpleDocTemplate(str(destination), pagesize=(8.27 * inch, 11.69 * inch),
                            rightMargin=0.58*inch, leftMargin=0.58*inch,
                            topMargin=0.55*inch, bottomMargin=0.58*inch)
    story = [p("M10 | Evidence and CPU-only sizing", "HeroM10"),
             p("PARTIAL EVIDENCE - no qualified production capacity or node count.", "BodyM10"),
             p("Model: Qwen3-ASR-0.6B CPU. Generated " + report["generated_utc"][:10] +
               ". The figures below are laptop prototype observations, not a server sizing claim.", "NoteM10"),
             p("Held-out FLEURS screen", "SectionM10"),
             p("Thirty completed calls: five clean / simulated-telephone pairs each for English, Indonesian and Mandarin. Error rates are failure-inclusive; WER and CER are never pooled. The sample is small and telephone audio is simulated."),]
    data = [["Language", "Metric", "Pairs", "Clean", "Sim. phone", "P50 first text"]]
    for language, item in report["holdout"]["by_language"].items():
        data.append([language.upper(), item["metric"].upper(), str(item["paired_clips"]),
                     f"{item['clean_rate']:.1%}", f"{item['simulated_telephone_rate']:.1%}",
                     f"{item['clean_p50_first_ms']/1000:.2f} s"])
    table = Table(data, colWidths=[1.06*inch, .65*inch, .5*inch, .72*inch, .94*inch, 1.12*inch], hAlign="LEFT")
    table.setStyle(TableStyle([("BACKGROUND", (0,0), (-1,0), colors.HexColor("#e9eff7")),
                               ("GRID", (0,0), (-1,-1), .35, colors.HexColor("#bdc8d6")),
                               ("VALIGN", (0,0), (-1,-1), "MIDDLE"),
                               ("FONTSIZE", (0,0), (-1,-1), 8),
                               ("TOPPADDING", (0,0), (-1,-1), 7),
                               ("BOTTOMPADDING", (0,0), (-1,-1), 7)]))
    story.extend([table, Spacer(1, 8),
                  p(f"All-condition first usable text: P50 {report['holdout']['overall_first_usable_p50_ms']/1000:.2f} s; "
                    f"P95 {report['holdout']['overall_first_usable_p95_ms']/1000:.2f} s. Descriptive only: conditions differ.", "NoteM10"),
                  p("Concurrency and long-form screens", "SectionM10")])
    capacity = report.get("capacity_screen")
    if capacity:
        first, second = capacity["rows"]
        story.append(p(f"Twenty 1.2-second English calls per point: one worker {first['completed_calls']}/20; "
                       f"two workers {second['completed_calls']}/20. Two-worker audio throughput was "
                       f"{capacity['throughput_ratio_2_vs_1']:.2f}x the one-worker screen, while P95 first text "
                       f"rose {capacity['first_p95_delta_ms_2_vs_1']:.0f} ms. No saturation knee or safe-node envelope was established."))
    else:
        screen = report["concurrency_screen"]
        story.append(p(f"Two workers, staggered short calls: {screen['completed_calls']}/{screen['offered_calls']} completed; "
                       f"P95 first text {screen['first_usable_p95_ms']/1000:.2f} s; sampled peak process-tree RSS "
                       f"{screen['peak_sampled_tree_rss_bytes']/2**30:.2f} GiB. Three 1.2-second clips cannot qualify tails or safe capacity."))
    stress = report["stress_screen"]
    story.append(p(f"Single-call synthetic replay: {stress['duration_seconds']:.2f} s, first text "
                   f"{stress['first_usable_ms']/1000:.2f} s. This is neither concurrent endurance nor conversational accuracy."))
    story.extend([p("Strict sub-second partials", "SectionM10"),
                  p(f"Not demonstrated. Held-out P50 first usable text was "
                    f"{report['strict_subsecond']['observed_holdout_p50_ms']/1000:.2f} s from stream start. "
                    "A 200 ms transport chunk does not imply 200 ms transcription latency."),
                  p("CPU-only sizing guide", "SectionM10"),
                  p("There is no measured saturation knee, sustained multi-call curve, chosen product SLO, or defensible headroom. Node count, vCPU and RAM are therefore unknown - no extrapolation is made.")])
    sizing = [["Concurrent legs", "Evidence class", "Node count / vCPU / RAM"]]
    sizing += [[str(x["target_legs"]), "Insufficient evidence", "Not estimated"] for x in report["sizing"]]
    table = Table(sizing, colWidths=[1.25*inch, 2.1*inch, 2.45*inch], hAlign="LEFT")
    table.setStyle(TableStyle([("BACKGROUND", (0,0), (-1,0), colors.HexColor("#e9eff7")),
                               ("GRID", (0,0), (-1,-1), .35, colors.HexColor("#bdc8d6")),
                               ("FONTSIZE", (0,0), (-1,-1), 8),
                               ("TOPPADDING", (0,0), (-1,-1), 5),
                               ("BOTTOMPADDING", (0,0), (-1,-1), 5)]))
    story.extend([table,
                  p("All rows: node specification, vCPU count, RAM, target RTF/P95 and headroom are unknown. "
                    "vCPU denotes one logical hardware thread, not an interchangeable unit between CPU models. "
                    "No identical-node extrapolation or failure reserve is assumed.", "NoteM10"),
                  p("Next evidence gates", "SectionM10")])
    for item in report["next_evidence"][:3]:
        story.append(p("- " + item, "NoteM10"))
    story.extend([PageBreak(), p("Configuration and provenance", "HeroM10")])
    if capacity:
        story.extend([p("Bounded native load curve", "SectionM10"),
                      p("Direct-mode CPU runs repeated the same 1.2-second English WAV twenty times per point. "
                        "The predeclared zero-failure and P95 send-lag <=1,000 ms thresholds are diagnostic only."),])
        curve_rows = [["Workers", "Calls", "Audio s / wall s", "P95 first", "P95 final", "Peak RSS"]]
        for item in capacity["rows"]:
            curve_rows.append([str(item["concurrency"]), f"{item['completed_calls']}/{item['calls']}",
                               f"{item['audio_seconds_per_wall_second']:.2f}",
                               f"{item['first_usable_p95_ms']/1000:.2f} s",
                               f"{item['final_p95_ms']/1000:.2f} s",
                               f"{item['sampled_peak_tree_rss_bytes']/2**30:.2f} GiB"])
        curve_table = Table(curve_rows, colWidths=[.68*inch, .68*inch, 1.32*inch, .92*inch, .92*inch, .95*inch], hAlign="LEFT")
        curve_table.setStyle(TableStyle([("BACKGROUND", (0,0), (-1,0), colors.HexColor("#e9eff7")),
                                         ("GRID", (0,0), (-1,-1), .35, colors.HexColor("#bdc8d6")),
                                         ("FONTSIZE", (0,0), (-1,-1), 8),
                                         ("TOPPADDING", (0,0), (-1,-1), 7),
                                         ("BOTTOMPADDING", (0,0), (-1,-1), 7)]))
        story.extend([curve_table, p("Source run IDs: " + "; ".join(item["run_id"] for item in capacity["rows"]), "NoteM10"),
                      p("Each point ran for less than one minute. The M6 diagnostic SLO flag is true, but there is no sustained multi-language load, failure boundary, thermal stability check, or production SLO.", "NoteM10")])
    story.extend([
                  p("The 4-thread versus 2-thread comparison is a sequential nine-clip tuning screen. It changes one model thread factor, but uses a fresh model process per call and has uncontrolled cache/thermal order; it is not a concurrent capacity comparison."),
                  p("Thread-factor screen", "SectionM10")])
    thread_rows = [["Language", "Clips", "4-thread P50 first", "2-thread P50 first", "Error-rate change"]]
    for language, item in report["threads_screen"]["by_language"].items():
        thread_rows.append([language.upper(), str(item["clips"]),
                            f"{item['left_p50_first_ms']/1000:.2f} s",
                            f"{item['right_p50_first_ms']/1000:.2f} s",
                            f"{item['right_error_rate'] - item['left_error_rate']:+.1%}"])
    thread_table = Table(thread_rows, colWidths=[.95*inch, .6*inch, 1.38*inch, 1.38*inch, 1.32*inch], hAlign="LEFT")
    thread_table.setStyle(TableStyle([("BACKGROUND", (0,0), (-1,0), colors.HexColor("#e9eff7")),
                                     ("GRID", (0,0), (-1,-1), .35, colors.HexColor("#bdc8d6")),
                                     ("FONTSIZE", (0,0), (-1,-1), 8),
                                     ("TOPPADDING", (0,0), (-1,-1), 7),
                                     ("BOTTOMPADDING", (0,0), (-1,-1), 7)]))
    story.extend([thread_table, p("Qualification boundaries", "SectionM10"),
                  p("Feasible production configurations cannot yet be identified: there are no declared latency, accuracy, failure, and memory thresholds. The 20-call suites qualify only for their narrow diagnostic send-lag/failure profile. The synthetic long-form run covers one session, not many live legs. A 50-per-language held-out cohort and real telephony/Common Voice remain unmeasured."),
                  p("The 50/100/200/500/1000-leg guide intentionally leaves node counts blank. A measured, sustained safe-leg envelope and justified headroom must precede any identical-node extrapolation; nominal laptop and server vCPU counts are not interchangeable."),
                  p("Source artifacts", "SectionM10"),
                  p(f"Held-out raw seal verified across {report['sources']['holdout_seal']['file_count']} files. "
                    "Load and stress screens are hash-recorded supporting artifacts, not sealed raw capacity runs.", "NoteM10")])
    for key in ("holdout_accuracy", "holdout_experiment", "load_summary", "stress_audit", "threads_comparison"):
        item = report["sources"][key]
        story.append(p(f"{key}: {item['path']} | SHA-256 {item['sha256'][:20]}...", "NoteM10"))
    story.append(p("Full paths, complete digests, run IDs and configuration hash are in report.json and report_metadata.json.", "NoteM10"))

    def footer(canvas, document):
        canvas.saveState()
        canvas.setFont("Helvetica", 8)
        canvas.setFillColor(colors.HexColor("#617082"))
        canvas.drawString(.58*inch, .34*inch, "ASR System | M10 evidence report")
        canvas.drawRightString(7.69*inch, .34*inch, f"Page {document.page}")
        canvas.restoreState()

    doc.build(story, onFirstPage=footer, onLaterPages=footer)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report_json", type=Path)
    parser.add_argument("output_pdf", type=Path)
    args = parser.parse_args()
    render(json.loads(args.report_json.read_text()), args.output_pdf)

#!/usr/bin/env python3
"""
WWV Log Analysis Script
Summarizes metadata and correlations from Phoenix SDR CSV logs.

Usage: python wwv_analyze.py [path_to_csv_folder]
       Default: current directory
"""

import sys
import csv
import os
from datetime import datetime
from collections import defaultdict

def parse_header(filepath):
    """Extract version and start time from CSV header."""
    info = {'version': 'unknown', 'started': 'unknown'}
    with open(filepath, 'r') as f:
        for line in f:
            if line.startswith('# Phoenix SDR'):
                parts = line.split(' v')
                if len(parts) > 1:
                    info['version'] = 'v' + parts[1].strip()
            elif line.startswith('# Started:'):
                info['started'] = line.replace('# Started:', '').strip()
            elif not line.startswith('#'):
                break
    return info

def analyze_sync(filepath):
    """Analyze sync detector log."""
    if not os.path.exists(filepath):
        return None
    
    info = parse_header(filepath)
    markers = []
    states = defaultdict(int)
    
    with open(filepath, 'r') as f:
        reader = csv.DictReader(filter(lambda x: not x.startswith('#'), f))
        for row in reader:
            markers.append(row)
            states[row['state']] += 1
    
    if not markers:
        return {'info': info, 'count': 0, 'states': {}, 'intervals': []}
    
    intervals = []
    for m in markers:
        interval = float(m['interval_sec'])
        if interval > 0:
            intervals.append(interval)
    
    return {
        'info': info,
        'count': len(markers),
        'states': dict(states),
        'intervals': intervals,
        'first_locked': next((i+1 for i, m in enumerate(markers) if m['state'] == 'LOCKED'), None),
        'avg_delta_ms': sum(float(m['delta_ms']) for m in markers) / len(markers),
        'missed': sum(1 for i in intervals if i > 90)
    }

def analyze_ticks(filepath):
    """Analyze tick detector log."""
    if not os.path.exists(filepath):
        return None
    
    info = parse_header(filepath)
    ticks = []
    markers = []
    
    with open(filepath, 'r') as f:
        reader = csv.DictReader(filter(lambda x: not x.startswith('#'), f))
        for row in reader:
            if row['tick_num'].startswith('M'):
                markers.append(row)
            elif row['tick_num'] not in ('META', 'GAIN'):
                ticks.append(row)
    
    if not ticks:
        return {'info': info, 'tick_count': 0, 'marker_count': len(markers), 'markers': []}
    
    intervals = []
    for t in ticks:
        try:
            interval = float(t['interval_ms'])
            if 500 < interval < 10000:
                intervals.append(interval)
        except:
            pass
    
    good_intervals = [i for i in intervals if 900 <= i <= 1100]
    
    return {
        'info': info,
        'tick_count': len(ticks),
        'marker_count': len(markers),
        'good_interval_pct': 100 * len(good_intervals) / len(intervals) if intervals else 0,
        'mean_interval_ms': sum(intervals) / len(intervals) if intervals else 0,
        'markers': [(m['tick_num'], float(m['timestamp_ms']), float(m['duration_ms'])) for m in markers]
    }

def analyze_markers(filepath):
    """Analyze marker detector log."""
    if not os.path.exists(filepath):
        return None
    
    info = parse_header(filepath)
    markers = []
    
    with open(filepath, 'r') as f:
        reader = csv.DictReader(filter(lambda x: not x.startswith('#'), f))
        for row in reader:
            if row['marker_num'].startswith('M'):
                markers.append(row)
    
    return {
        'info': info,
        'count': len(markers),
        'markers': [(m['marker_num'], float(m['timestamp_ms']), float(m['duration_ms'])) for m in markers]
    }

def analyze_subcarrier(filepath):
    """Analyze subcarrier log."""
    if not os.path.exists(filepath):
        return None
    
    info = parse_header(filepath)
    total = 0
    matches = 0
    by_expected = defaultdict(lambda: {'total': 0, 'match': 0})
    
    with open(filepath, 'r') as f:
        reader = csv.DictReader(filter(lambda x: not x.startswith('#'), f))
        for row in reader:
            total += 1
            expected = row['expected']
            match = row['match']
            
            by_expected[expected]['total'] += 1
            if match == 'YES':
                matches += 1
                by_expected[expected]['match'] += 1
    
    return {
        'info': info,
        'total': total,
        'matches': matches,
        'match_pct': 100 * matches / total if total else 0,
        'by_expected': dict(by_expected)
    }

def analyze_channel(filepath):
    """Analyze channel quality log."""
    if not os.path.exists(filepath):
        return None
    
    info = parse_header(filepath)
    quality_counts = defaultdict(int)
    snr_values = []
    
    with open(filepath, 'r') as f:
        reader = csv.DictReader(filter(lambda x: not x.startswith('#'), f))
        for row in reader:
            try:
                quality_counts[row['quality']] += 1
                snr_values.append(float(row['snr_db']))
            except:
                pass
    
    return {
        'info': info,
        'quality': dict(quality_counts),
        'snr_min': min(snr_values) if snr_values else 0,
        'snr_max': max(snr_values) if snr_values else 0,
        'snr_avg': sum(snr_values) / len(snr_values) if snr_values else 0
    }

def print_report(base_path='.'):
    """Print full analysis report."""
    
    # Find files - prefer most recent if multiple exist
    files = {
        'sync': None,
        'ticks': None,
        'markers': None,
        'subcarrier': None,
        'channel': None
    }
    
    candidates = defaultdict(list)
    for f in os.listdir(base_path):
        if not f.endswith('.csv'):
            continue
        full_path = os.path.join(base_path, f)
        mtime = os.path.getmtime(full_path)
        
        if 'sync' in f:
            candidates['sync'].append((mtime, full_path))
        elif 'ticks' in f:
            candidates['ticks'].append((mtime, full_path))
        elif 'markers' in f:
            candidates['markers'].append((mtime, full_path))
        elif 'subcarrier' in f:
            candidates['subcarrier'].append((mtime, full_path))
        elif 'channel' in f:
            candidates['channel'].append((mtime, full_path))
    
    # Pick most recent of each type
    for key in files:
        if candidates[key]:
            candidates[key].sort(reverse=True)
            files[key] = candidates[key][0][1]
    
    print("=" * 60)
    print("WWV LOG ANALYSIS REPORT")
    print("=" * 60)
    
    # Sync analysis
    sync = analyze_sync(files['sync']) if files['sync'] else None
    if sync:
        print(f"\n## SYNC DETECTOR ({sync['info']['version']})")
        print(f"   Started: {sync['info']['started']}")
        print(f"   Confirmed markers: {sync['count']}")
        print(f"   States: {sync['states']}")
        if sync['first_locked']:
            print(f"   First LOCKED: marker #{sync['first_locked']}")
        if sync['count'] > 0:
            print(f"   Avg correlation delta: {sync['avg_delta_ms']:.0f} ms")
        print(f"   Missed markers (>90s gap): {sync['missed']}")
        if sync['intervals']:
            print(f"   Interval range: {min(sync['intervals']):.1f}s - {max(sync['intervals']):.1f}s")
    
    # Tick analysis
    ticks = analyze_ticks(files['ticks']) if files['ticks'] else None
    if ticks:
        print(f"\n## TICK DETECTOR")
        print(f"   Ticks detected: {ticks['tick_count']}")
        print(f"   Minute markers: {ticks['marker_count']}")
        print(f"   Good intervals (900-1100ms): {ticks['good_interval_pct']:.1f}%")
        print(f"   Mean interval: {ticks['mean_interval_ms']:.0f} ms")
    
    # Marker analysis
    markers = analyze_markers(files['markers']) if files['markers'] else None
    if markers:
        print(f"\n## MARKER DETECTOR")
        print(f"   Markers detected: {markers['count']}")
    
    # Correlation check - match by timestamp proximity, not marker number
    if ticks and markers and ticks['markers'] and markers['markers']:
        print(f"\n## CORRELATION CHECK (by timestamp, 1500ms window)")
        tick_list = sorted(ticks['markers'], key=lambda x: x[1])  # (name, timestamp, duration)
        marker_list = sorted(markers['markers'], key=lambda x: x[1])
        
        correlated = 0
        tick_only = 0
        marker_only = 0
        used_markers = set()
        
        # Limit display to first 20, but count all
        display_limit = 20
        displayed = 0
        
        for t_name, t_time, t_dur in tick_list:
            # Find closest marker within window
            best_match = None
            best_delta = 1500
            for i, (m_name, m_time, m_dur) in enumerate(marker_list):
                if i in used_markers:
                    continue
                delta = abs(t_time - m_time)
                if delta < best_delta:
                    best_delta = delta
                    best_match = (i, m_name, m_time, m_dur)
            
            if best_match:
                used_markers.add(best_match[0])
                correlated += 1
                if displayed < display_limit:
                    print(f"   {t_name}↔{best_match[1]}: delta={best_delta:.0f}ms ✓")
                    displayed += 1
            else:
                tick_only += 1
                if displayed < display_limit:
                    print(f"   {t_name}: tick@{t_time:.0f}ms (no marker)")
                    displayed += 1
        
        if displayed >= display_limit:
            print(f"   ... ({len(tick_list) - display_limit} more)")
        
        # Count unmatched markers
        marker_only = len(marker_list) - len(used_markers)
        
        print(f"\n   Summary: {correlated} correlated, {tick_only} tick-only, {marker_only} marker-only")
        if sync:
            print(f"   Sync confirmed: {sync['count']} (with interval validation)")
    
    # Channel analysis
    channel = analyze_channel(files['channel']) if files['channel'] else None
    if channel:
        print(f"\n## CHANNEL QUALITY")
        print(f"   Quality distribution: {channel['quality']}")
        print(f"   SNR range: {channel['snr_min']:.1f} - {channel['snr_max']:.1f} dB")
        print(f"   SNR average: {channel['snr_avg']:.1f} dB")
    
    # Subcarrier analysis
    sub = analyze_subcarrier(files['subcarrier']) if files['subcarrier'] else None
    if sub:
        print(f"\n## SUBCARRIER DETECTION")
        print(f"   Total samples: {sub['total']}")
        print(f"   Match rate: {sub['match_pct']:.1f}%")
        for exp, counts in sub['by_expected'].items():
            pct = 100 * counts['match'] / counts['total'] if counts['total'] else 0
            print(f"   {exp}: {counts['match']}/{counts['total']} ({pct:.0f}%)")
    
    print("\n" + "=" * 60)

if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else '.'
    print_report(path)

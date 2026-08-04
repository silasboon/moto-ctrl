/**
 * Firmware version and uptime, shown at the top of Settings.
 *
 * These used to sit on the Ride screen. They're the kind of thing you read
 * once when something is wrong — before reporting a fault, or to check an
 * update took — not while pulling gloves on, so they were taking room from
 * the two controls that screen exists for.
 */
import React, { useEffect, useState } from 'react';
import { StyleSheet, Text, View } from 'react-native';

import type { MotoClient } from '../protocol/MotoClient';
import type { Status } from '../protocol/types';
import { Card } from '../ui/components';
import { space, type } from '../ui/theme';

/** Uptime as a rider would say it, biggest unit first. */
function formatUptime(ms: number): string {
  const total = Math.floor(ms / 1000);
  const d = Math.floor(total / 86400);
  const h = Math.floor((total % 86400) / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  if (d > 0) return `${d}d ${h}h`;
  if (h > 0) return `${h}h ${m}m`;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}

export function BoardInfoCard({
  client,
}: {
  client: MotoClient;
}): React.JSX.Element {
  const [status, setStatus] = useState<Status | null>(client.getLastStatus());
  useEffect(() => client.onStatus(setStatus), [client]);

  return (
    <Card>
      <View style={styles.row}>
        <Text style={styles.label}>Firmware</Text>
        <Text style={styles.value}>
          {status
            ? `${status.fwMajor}.${status.fwMinor}.${status.fwPatch}`
            : '—'}
        </Text>
      </View>
      <View style={styles.row}>
        <Text style={styles.label}>Uptime</Text>
        <Text style={styles.value}>
          {status ? formatUptime(status.uptimeMs) : '—'}
        </Text>
      </View>
    </Card>
  );
}

const styles = StyleSheet.create({
  row: {
    flexDirection: 'row',
    alignItems: 'baseline',
    justifyContent: 'space-between',
    gap: space.md,
  },
  label: { ...type.overline },
  value: { ...type.valueSmall, fontSize: 14 },
});

/**
 * Event log viewer (docs/PROTOCOL.md §15): the persisted ring
 * buffer of security/safety-relevant events (lock transitions, key enroll/
 * revoke/transfer, factory reset, cheat-code lockout, OTA begin/success/
 * failure, low-voltage cutoff). Read-only — there is no wire op to clear or
 * edit it from the app, by design (see mc_event_log.h).
 */
import React, { useCallback, useEffect, useState } from 'react';
import {
  FlatList,
  StyleSheet,
  Text,
  TouchableOpacity,
  View,
} from 'react-native';

import {
  EVENT_UNLOCK_METHOD_NAMES,
  EVENT_TYPE,
  eventTypeName,
} from '../protocol/constants';
import type { MotoClient } from '../protocol/MotoClient';
import type { EventRecord } from '../protocol/types';
import { Screen } from '../ui/components';
import { colors } from '../ui/theme';

interface Props {
  client: MotoClient;
  onDone: () => void;
}

function describeEvent(record: EventRecord): string {
  const name = eventTypeName(record.type);
  switch (record.type) {
    case EVENT_TYPE.LOCK_RELEASED:
      return `${name} (${EVENT_UNLOCK_METHOD_NAMES[record.arg0] ?? `method ${record.arg0}`})`;
    case EVENT_TYPE.KEY_ENROLLED:
    case EVENT_TYPE.KEY_REVOKED:
      return `${name} (slot ${record.arg0})`;
    case EVENT_TYPE.CHEATCODE_LOCKOUT:
      return `${name} (${record.arg0} wrong attempts)`;
    case EVENT_TYPE.OTA_FAILURE:
      return `${name} (code ${record.arg0})`;
    default:
      return name;
  }
}

function formatUptime(ms: number): string {
  const totalSeconds = Math.floor(ms / 1000);
  const h = Math.floor(totalSeconds / 3600);
  const m = Math.floor((totalSeconds % 3600) / 60);
  const s = totalSeconds % 60;
  return h > 0 ? `${h}h${m}m${s}s` : m > 0 ? `${m}m${s}s` : `${s}s`;
}

export function EventLogScreen({ client, onDone }: Props): React.JSX.Element {
  const [records, setRecords] = useState<EventRecord[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  const refresh = useCallback(() => {
    setLoading(true);
    setError(null);
    client
      .getEventLog(0)
      .then(r => setRecords([...r].reverse())) // newest-first for display
      .catch((err: unknown) =>
        setError(err instanceof Error ? err.message : String(err)),
      )
      .finally(() => setLoading(false));
  }, [client]);

  useEffect(() => {
    refresh();
  }, [refresh]);

  return (
    <Screen title="Event Log" onBack={onDone} scroll={false}>
      <TouchableOpacity
        style={styles.refreshButton}
        onPress={refresh}
        disabled={loading}
      >
        <Text style={styles.refreshButtonText}>
          {loading ? 'Loading…' : 'Refresh'}
        </Text>
      </TouchableOpacity>
      {error && <Text style={styles.error}>{error}</Text>}
      {!loading && !error && records.length === 0 && (
        <Text style={styles.hint}>No events recorded yet.</Text>
      )}
      <FlatList
        data={records}
        keyExtractor={r => String(r.seq)}
        contentContainerStyle={styles.listContent}
        renderItem={({ item }) => (
          <View style={styles.row}>
            <Text style={styles.rowSeq}>#{item.seq}</Text>
            <View style={styles.rowBody}>
              <Text style={styles.rowText}>{describeEvent(item)}</Text>
              <Text style={styles.rowMeta}>
                at {formatUptime(item.uptimeMs)} uptime
              </Text>
            </View>
          </View>
        )}
      />
    </Screen>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: colors.bg, padding: 16, gap: 10 },
  header: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
  },
  title: { color: colors.text, fontSize: 20, fontWeight: '700' },
  link: { color: colors.accent },
  hint: { fontSize: 12, color: colors.textMuted },
  error: { color: colors.danger },
  refreshButton: {
    padding: 10,
    borderRadius: 8,
    borderWidth: 1,
    borderColor: colors.accent,
    alignItems: 'center',
  },
  refreshButtonText: { color: colors.accent, fontWeight: '600' },
  listContent: { gap: 6, paddingVertical: 8 },
  row: {
    flexDirection: 'row',
    gap: 10,
    padding: 10,
    borderWidth: 1,
    borderColor: colors.border,
    borderRadius: 8,
  },
  rowSeq: { fontFamily: 'Menlo', color: colors.textFaint, width: 44 },
  rowBody: { flex: 1 },
  rowText: { color: colors.text, fontWeight: '600' },
  rowMeta: { fontSize: 11, color: colors.textFaint },
});

/**
 * Board: settings about the board itself rather than about a channel or a
 * button. Right now that is one thing — its name.
 *
 * The name is an ordinary config field (schema_version 8), so it saves,
 * exports and restores through the same CONFIG channel as everything else,
 * and an ownership transfer resets it along with the keys.
 */
import React, { useEffect, useState } from 'react';

import { DEVICE_NAME } from '../protocol/constants';
import type { MotoClient } from '../protocol/MotoClient';
import type { DeviceConfig } from '../protocol/types';
import {
  Button,
  Card,
  Field,
  SkeletonScreen,
  Notice,
  Screen,
  SectionHeader,
  useLeaveGuard,
} from '../ui/components';

interface Props {
  client: MotoClient;
  onDone: () => void;
}

/** Matches MC_DEVICE_NAME_MAX (24) less the NUL the firmware reserves. */
const NAME_MAX = 23;

export function BoardScreen({ client, onDone }: Props): React.JSX.Element {
  const [config, setConfig] = useState<DeviceConfig | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);
  const [baseline, setBaseline] = useState<string | null>(null);

  useEffect(() => {
    client
      .configRead()
      .then(c => {
        setConfig(c);
        setBaseline(JSON.stringify(c));
      })
      .catch((err: unknown) =>
        setError(err instanceof Error ? err.message : String(err)),
      )
      .finally(() => setLoading(false));
  }, [client]);

  const dirty = config !== null && JSON.stringify(config) !== baseline;
  const back = useLeaveGuard(dirty, onDone);

  async function save(): Promise<void> {
    if (!config) return;
    setSaving(true);
    setError(null);
    setSaved(false);
    try {
      const result = await client.configWrite(config);
      if (result.ok) {
        setSaved(true);
        setBaseline(JSON.stringify(config));
      } else {
        setError(`Device rejected the config: ${result.resultName}`);
      }
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      setSaving(false);
    }
  }

  if (loading) return <SkeletonScreen title="Board" onBack={back} cards={1} />;
  if (!config) {
    return (
      <Screen title="Board" onBack={back}>
        <Notice tone="danger">
          {error ?? 'Could not read the configuration from the device.'}
        </Notice>
      </Screen>
    );
  }

  return (
    <Screen
      title="Board"
      onBack={back}
      trailing={
        <Button
          label={saving ? 'Saving' : 'Save'}
          onPress={save}
          busy={saving}
          tone={dirty ? 'primary' : 'secondary'}
        />
      }
    >
      {error && <Notice tone="danger">{error}</Notice>}
      {saved && !dirty && <Notice tone="on">Saved to the device.</Notice>}

      <SectionHeader hint="What this board calls itself when your phone looks for it.">
        Name
      </SectionHeader>
      <Card>
        <Field
          label="Board name"
          value={config.device_name}
          onChangeText={v => {
            setConfig(prev => (prev ? { ...prev, device_name: v } : prev));
            setSaved(false);
          }}
          placeholder={DEVICE_NAME}
          maxLength={NAME_MAX}
          autoCapitalize="words"
          autoCorrect={false}
          hint={`Leave it empty to go back to ${DEVICE_NAME}. Useful when you run more than one bike, or to tell your board apart from someone else's at a meet.`}
        />
      </Card>

      {/* Phones cache the advertised name, so the pairing list can keep
       * showing the old one for a while after a rename. Saying so here is
       * cheaper than a rider assuming the save silently failed. */}
      <Notice tone="info">
        The new name reaches your phone the next time it discovers the board. If
        the old one lingers in the list, disconnect and scan again.
      </Notice>
    </Screen>
  );
}

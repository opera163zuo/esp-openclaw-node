import { createEffect, createSignal, type Component } from 'solid-js';
import type { AppConfig } from '../api/client';
import { fetchConfigGroups, saveConfigPatch } from '../api/client';
import { LanguageSwitcher } from '../components/layout/LanguageSwitcher';
import { Banner } from '../components/ui/Banner';
import { Button } from '../components/ui/Button';
import { TextInput } from '../components/ui/FormField';
import { t } from '../i18n';
import { patchConfigLocal } from '../state/config';
import { pushToast } from '../state/toast';
import type { TabId } from '../state/dirty';

type SetupWizardPageProps = {
  onRestartRequest: (targetTab: TabId) => void;
};

type SetupForm = {
  wifi_ssid: string;
  wifi_password: string;
  openclaw_gateway_url: string;
  openclaw_gateway_token: string;
  openclaw_device_family: string;
};

export const SetupWizardPage: Component<SetupWizardPageProps> = (props) => {
  const [form, setForm] = createSignal<SetupForm>({
    wifi_ssid: '',
    wifi_password: '',
    openclaw_gateway_url: '',
    openclaw_gateway_token: '',
    openclaw_device_family: 'm5stack-sticks3',
  });
  const [error, setError] = createSignal<string | null>(null);
  const [saving, setSaving] = createSignal(false);

  createEffect(() => {
    void fetchConfigGroups(['wifi', 'openclaw']).then((config) => {
      setForm((current) => ({
        ...current,
        wifi_ssid: config.wifi_ssid ?? current.wifi_ssid,
        wifi_password: config.wifi_password ?? current.wifi_password,
        openclaw_gateway_url: config.openclaw_gateway_url ?? current.openclaw_gateway_url,
        openclaw_gateway_token: config.openclaw_gateway_token ?? current.openclaw_gateway_token,
        openclaw_device_family: config.openclaw_device_family ?? current.openclaw_device_family,
      }));
    }).catch((err) => setError((err as Error).message));
  });

  const update = (key: keyof SetupForm, value: string) =>
    setForm((current) => ({ ...current, [key]: value }));

  const save = async () => {
    const current = form();
    if (!current.wifi_ssid.trim()) {
      setError(t('wifiValidationSsidRequired') as string);
      return;
    }
    if (current.wifi_password && current.wifi_password.length < 8) {
      setError(t('wifiValidationPasswordLength') as string);
      return;
    }
    if (current.openclaw_gateway_url && !/^wss?:\/\//i.test(current.openclaw_gateway_url.trim())) {
      setError(t('openclawGatewayUrlInvalid') as string);
      return;
    }

    setSaving(true);
    setError(null);
    const patch: Partial<AppConfig> = {
      wifi_ssid: current.wifi_ssid.trim(),
      wifi_password: current.wifi_password,
      openclaw_gateway_url: current.openclaw_gateway_url.trim(),
      openclaw_gateway_token: current.openclaw_gateway_token,
      openclaw_device_family: current.openclaw_device_family.trim() || 'esp32',
    };
    try {
      await saveConfigPatch(patch);
      patchConfigLocal(patch);
      pushToast(t('saveSuccess') as string, 'success', 3500);
      props.onRestartRequest('basic');
    } catch (err) {
      setError((err as Error).message);
    } finally {
      setSaving(false);
    }
  };

  return (
    <div class="min-h-screen bg-[var(--color-bg)] text-[var(--color-text-primary)] p-5 sm:p-8">
      <div class="max-w-2xl mx-auto space-y-6">
        <div class="flex justify-between items-center">
          <div>
            <p class="text-xs uppercase tracking-[0.18em] text-[var(--color-accent)]">ESP-OpenClaw-Node</p>
            <h1 class="text-2xl font-semibold mt-2">{t('setupTitle')}</h1>
            <p class="text-sm text-[var(--color-text-secondary)] mt-2">{t('setupDescription')}</p>
          </div>
          <LanguageSwitcher />
        </div>

        {error() && <Banner kind="error" message={error()!} />}

        <section class="rounded-[var(--radius-lg)] border border-[var(--color-border-subtle)] bg-[var(--color-surface)] p-5 space-y-4">
          <h2 class="text-lg font-medium">{t('sectionWifi')}</h2>
          <TextInput label={t('wifiSsid')} value={form().wifi_ssid} onInput={(e) => update('wifi_ssid', e.currentTarget.value)} />
          <TextInput type="password" label={t('wifiPassword')} value={form().wifi_password} onInput={(e) => update('wifi_password', e.currentTarget.value)} />
        </section>

        <section class="rounded-[var(--radius-lg)] border border-[var(--color-border-subtle)] bg-[var(--color-surface)] p-5 space-y-4">
          <h2 class="text-lg font-medium">{t('sectionOpenClaw')}</h2>
          <p class="text-sm text-[var(--color-text-secondary)]">{t('setupOpenclawDescription')}</p>
          <TextInput full label={t('openclawGatewayUrl')} placeholder="wss://gateway.example/ws" value={form().openclaw_gateway_url} onInput={(e) => update('openclaw_gateway_url', e.currentTarget.value)} />
          <TextInput type="password" label={t('openclawGatewayToken')} value={form().openclaw_gateway_token} onInput={(e) => update('openclaw_gateway_token', e.currentTarget.value)} />
          <TextInput label={t('openclawDeviceFamily')} value={form().openclaw_device_family} onInput={(e) => update('openclaw_device_family', e.currentTarget.value)} />
          <p class="text-xs text-[var(--color-text-muted)]">{t('setupPairingHint')}</p>
        </section>

        <div class="flex justify-end">
          <Button disabled={saving()} onClick={() => void save()}>{saving() ? t('saving') : t('saveBtn')}</Button>
        </div>
      </div>
    </div>
  );
};

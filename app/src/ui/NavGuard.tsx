/**
 * Lets whatever is on screen veto a navigation the shell wants to perform.
 *
 * The tab bar stays visible on detail screens, which means a rider can tab
 * away from a half-edited config without ever touching its Back button — and
 * `useLeaveGuard`'s confirmation, which is wired to Back, would never fire.
 * The edits would just be gone.
 *
 * So the shell doesn't navigate directly: it hands the intended navigation to
 * `run()`, and whichever screen is mounted gets to decide whether it happens
 * now, after a confirmation, or not at all. A screen with nothing to protect
 * registers nothing and `run()` proceeds immediately, which is the case for
 * every screen that only reads.
 *
 * One guard at a time, deliberately: exactly one detail screen is ever
 * mounted, and a stack of guards would mean a stack of confirmations for a
 * single tap.
 */
import React from 'react';

/** Called with the navigation the shell wants to perform. Invoke `proceed`
 * to allow it; do nothing to cancel. */
export type NavGuard = (proceed: () => void) => void;

interface NavGuardApi {
  /** Registers the mounted screen's guard, or clears it with null. */
  register: (guard: NavGuard | null) => void;
  /** Performs `proceed`, subject to the registered guard. */
  run: (proceed: () => void) => void;
}

const NavGuardContext = React.createContext<NavGuardApi | null>(null);

export function NavGuardProvider({
  children,
}: {
  children: React.ReactNode;
}): React.JSX.Element {
  /* A ref, not state: registering a guard must not re-render the shell, and
   * `run` needs the guard as it is at the moment of the tap rather than
   * whatever a closure captured on the last render. */
  const guard = React.useRef<NavGuard | null>(null);

  const api = React.useMemo<NavGuardApi>(
    () => ({
      register: next => {
        guard.current = next;
      },
      run: proceed => {
        const current = guard.current;
        if (current) current(proceed);
        else proceed();
      },
    }),
    [],
  );

  return (
    <NavGuardContext.Provider value={api}>{children}</NavGuardContext.Provider>
  );
}

/** Null outside a provider — screens rendered standalone (tests) simply have
 * nothing to register with. */
export function useNavGuard(): NavGuardApi | null {
  return React.useContext(NavGuardContext);
}

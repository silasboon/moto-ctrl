/**
 * Left-edge swipe to go back.
 *
 * Hand-rolled on PanResponder because this app has no navigator — the gesture
 * normally comes free with react-navigation's native stack, and pulling that
 * in (plus react-native-screens and react-native-gesture-handler) for one
 * gesture is a lot of native surface for what is 40 lines here.
 *
 * Only claims the gesture when a drag STARTS inside the edge strip and is
 * clearly horizontal. Anything else — a tap, a vertical scroll, a horizontal
 * drag beginning mid-screen — is left to the view underneath, so the chip
 * rows and scroll views keep working exactly as they did.
 */
import React from 'react';
import {
  PanResponder,
  Platform,
  StyleSheet,
  View,
  type ViewProps,
} from 'react-native';

/** How far in from the left edge a back-swipe may start. Matches roughly what
 * UIKit's interactive pop gesture uses. */
const EDGE_WIDTH = 24;
/** Horizontal travel before the gesture counts as a back. */
const TRIGGER_DX = 56;
/** Beyond this much vertical travel it's a scroll, not a back. */
const MAX_DY = 40;

interface Props extends ViewProps {
  onBack: () => void;
  children: React.ReactNode;
}

export function EdgeSwipeBack({
  onBack,
  children,
  style,
  ...rest
}: Props): React.JSX.Element {
  /* Held in a ref so the responder — created once — always calls the current
   * handler rather than the one captured on first render. */
  const onBackRef = React.useRef(onBack);
  React.useEffect(() => {
    onBackRef.current = onBack;
  }, [onBack]);

  const responder = React.useMemo(
    () =>
      PanResponder.create({
        onMoveShouldSetPanResponder: (event, gesture) => {
          /* pageX at the START of the drag: gesture.moveX has already moved.
           * Subtracting dx recovers where the finger went down. */
          const startX = event.nativeEvent.pageX - gesture.dx;
          return (
            startX <= EDGE_WIDTH &&
            gesture.dx > 12 &&
            Math.abs(gesture.dy) < Math.abs(gesture.dx)
          );
        },
        onPanResponderRelease: (_event, gesture) => {
          if (gesture.dx >= TRIGGER_DX && Math.abs(gesture.dy) <= MAX_DY) {
            onBackRef.current();
          }
        },
      }),
    [],
  );

  /* Android has a system back gesture of its own, already handled through
   * BackHandler in useLeaveGuard — adding a second one here would mean two
   * back gestures racing on the same edge. */
  if (Platform.OS !== 'ios') {
    return (
      <View style={[styles.fill, style]} {...rest}>
        {children}
      </View>
    );
  }

  return (
    <View style={[styles.fill, style]} {...responder.panHandlers} {...rest}>
      {children}
    </View>
  );
}

const styles = StyleSheet.create({
  fill: { flex: 1 },
});

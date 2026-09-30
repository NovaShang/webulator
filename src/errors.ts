export class WebulatorError extends Error {
  constructor(message: string) { super(message); this.name = new.target.name; }
}
/** The profile is invalid, or a parameter is outside what it allows. */
export class ProfileError extends WebulatorError {}
/** A ROM, disk, core or snapshot could not be fetched, or its checksum is wrong. */
export class AssetError extends WebulatorError {}
/** The snapshot was made by a different core build. */
export class BuildMismatchError extends WebulatorError {}
/** The core did not reach a safe point in time. */
export class SnapshotTimeoutError extends WebulatorError {}
/** The input is beyond what the emulated machine has (e.g. a right click on a one-button Mac). */
export class UnsupportedInputError extends WebulatorError {}
/** The core trapped or aborted. The machine is now "crashed". */
export class CoreCrashedError extends WebulatorError {}

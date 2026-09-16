// The thing tsc is pointed at. Deliberately tiny: what is under test is the
// compiler, not this file.
//
// The assignment on line 12 is a deliberate type error -- a string into a
// `number`, which TypeScript reports as TS2322. run.sh asserts on the code
// AND on the position, so moving these lines means updating run.sh too.

export function area(width: number, height: number): number {
  return width * height;
}

const height: number = "4";

export const twelve: number = area(3, Number(height));

// The enter/leave transitions from one hover chain to another, each chain
// listed from the hit element outward. Leaving is innermost first; entering is
// outermost first, the order a browser uses for one pointer movement.
export function hoverTransition<T>(from: readonly T[], to: readonly T[]) {
    return {
        leaving: from.filter((element) => !to.includes(element)),
        entering: to.filter((element) => !from.includes(element)).reverse(),
    };
}

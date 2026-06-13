import { Annotation } from "../internal";
import type { ClassMethodDecorator } from "../types/decorator_fills";
export declare const FLOW = "flow";
export declare class FlowCancellationError extends Error {
    constructor();
    toString(): string;
}
export declare function isFlowCancellationError(error: Error): error is FlowCancellationError;
export type CancellablePromise<T> = Promise<T> & {
    cancel(): void;
};
type FlowGenerator = (...args: any[]) => Generator<any, any, any> | AsyncGenerator<any, any, any>;
interface Flow extends Annotation, PropertyDecorator {
    <This, Value extends FlowGenerator>(value: Value, context: ClassMethodDecoratorContext<This, Value>): Value | void;
    <R, Args extends any[]>(generator: (...args: Args) => Generator<any, R, any> | AsyncGenerator<any, R, any>): (...args: Args) => CancellablePromise<R>;
    bound: Annotation & PropertyDecorator & ClassMethodDecorator;
}
export declare const flow: Flow;
export declare function flowResult<T>(result: T): T extends Generator<any, infer R, any> ? CancellablePromise<R> : T extends CancellablePromise<any> ? T : never;
export declare function isFlow(fn: any): boolean;
export {};

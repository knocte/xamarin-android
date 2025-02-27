using System;
using System.Collections.Generic;
using System.Linq;
using Mono.Cecil;
using Mono.Linker;
using Mono.Linker.Steps;
using Mono.Tuner;
using Xamarin.Android.Tasks;

namespace MonoDroid.Tuner {

	public class MarkJavaObjects :
#if NET5_LINKER
		BaseMarkHandler
#else   // !NET5_LINKER
		BaseSubStep
#endif  // !NET5_LINKER
	{
		Dictionary<ModuleDefinition, Dictionary<string, TypeDefinition>> module_types = new Dictionary<ModuleDefinition, Dictionary<string, TypeDefinition>> ();

#if NET5_LINKER
		public override void Initialize (LinkContext context, MarkContext markContext)
		{
			base.Initialize (context, markContext);
			markContext.RegisterMarkTypeAction (type => ProcessType (type));
		}
#else   // !NET5_LINKER
		public override SubStepTargets Targets {
			get { return SubStepTargets.Type; }
		}
#endif  // !NET5_LINKER

		public
#if !NET5_LINKER
		override
#endif  // !NET5_LINKER
		void ProcessType (TypeDefinition type)
		{
			// If this isn't a JLO or IJavaObject implementer,
			// then we don't need to MarkJavaObjects
			if (!type.ImplementsIJavaObject ())
				return;

			PreserveJavaObjectImplementation (type);

			if (IsImplementor (type))
				PreserveImplementor (type);

			// If a user overrode a method, we need to preserve it,
			// because it won't be referenced anywhere, but it will
			// be called from Java
			if (IsUserType (type) && type.HasMethods) {
				foreach (var method in type.Methods.Where (m => m.Overrides != null))
					PreserveMethod (type, method);
			}
		}

		void PreserveJavaObjectImplementation (TypeDefinition type)
		{
			PreserveIntPtrConstructor (type);
			PreserveAttributeSetConstructor (type);
			PreserveInvoker (type);
		}

		void PreserveAttributeSetConstructor (TypeDefinition type)
		{
			if (!type.HasMethods)
				return;

			foreach (var constructor in GetAttributeSetConstructors (type))
				PreserveMethod (type, constructor);
		}

		static IEnumerable<MethodDefinition> GetAttributeSetConstructors (TypeDefinition type)
		{
			foreach (MethodDefinition constructor in type.Methods.Where (m => m.IsConstructor)) {
				if (!constructor.HasParameters)
					continue;

				var parameters = constructor.Parameters;

				if (parameters.Count < 2 || parameters.Count > 3)
					continue;

				if (parameters [0].ParameterType.FullName != "Android.Content.Context")
					continue;

				if (parameters [1].ParameterType.FullName != "Android.Util.IAttributeSet")
					continue;

				if (parameters.Count == 3 && parameters [2].ParameterType.FullName != "System.Int32")
					continue;

				yield return constructor;
			}
		}

		void PreserveIntPtrConstructor (TypeDefinition type)
		{
			var constructor = GetIntPtrConstructor (type);

			if (constructor != null)
				PreserveMethod (type, constructor);

			var constructor2 = GetNewIntPtrConstructor (type);

			if (constructor2 != null)
				PreserveMethod (type, constructor2);
		}

		static MethodDefinition GetIntPtrConstructor (TypeDefinition type)
		{
			if (!type.HasMethods)
				return null;

			foreach (MethodDefinition constructor in type.Methods.Where (m => m.IsConstructor)) {
				if (!constructor.HasParameters)
					continue;

				if (constructor.Parameters.Count != 1 || constructor.Parameters[0].ParameterType.FullName != "System.IntPtr")
					continue;

				return constructor;
			}

			return null;
		}

		static MethodDefinition GetNewIntPtrConstructor (TypeDefinition type)
		{
			if (!type.HasMethods)
				return null;

			foreach (MethodDefinition constructor in type.Methods.Where (m => m.IsConstructor)) {
				if (!constructor.HasParameters)
					continue;

				if (constructor.Parameters.Count != 2 || constructor.Parameters[0].ParameterType.FullName != "System.IntPtr"
					|| constructor.Parameters[1].ParameterType.FullName != "Android.Runtime.JniHandleOwnership")
					continue;

				return constructor;
			}

			return null;
		}

		void PreserveMethod (TypeDefinition type, MethodDefinition method)
		{
			Annotations.AddPreservedMethod (type, method);
		}

		string TypeNameWithoutKey (string name)
		{
			var idx = name.IndexOf (", PublicKeyToken=");
			if (idx > 0)
				name = name.Substring (0, idx);

			return name;
		}

		bool CheckInvokerType (TypeDefinition type, string name)
		{
			return TypeNameWithoutKey (name) == TypeNameWithoutKey ($"{ type.FullName}, { type.Module.Assembly.FullName}");
		}

		void PreserveInterfaceMethods (TypeDefinition type, TypeDefinition invoker)
		{
			foreach (var m in type.Methods.Where (m => !m.IsConstructor)) {
				string methodAndType;
				if (!m.TryGetRegisterMember (out methodAndType))
					continue;

				if (!methodAndType.Contains (":"))
					continue;

				var values = methodAndType.Split (new char [] { ':' }, 2);
				if (!CheckInvokerType (invoker, values [1]))
					continue;

				foreach (var invokerMethod in invoker.Methods.Where (m => !m.IsConstructor)) {
					if (invokerMethod.Name == values [0]) {
						PreserveMethod (invoker, invokerMethod);
						break;
					}
				}
			}
		}

		void PreserveInvoker (TypeDefinition type)
		{
			var invoker = GetInvokerType (type);
			if (invoker == null)
				return;

			PreserveConstructors (type, invoker);
			PreserveIntPtrConstructor (invoker);
			PreserveInterfaceMethods (type, invoker);
		}

		TypeDefinition GetInvokerType (TypeDefinition type)
		{
			const string suffix = "Invoker";
			string fullname = type.FullName;

			if (type.HasGenericParameters) {
				var pos = fullname.IndexOf ('`');
				if (pos == -1)
					throw new ArgumentException ();

				fullname = fullname.Substring (0, pos) + suffix + fullname.Substring (pos);
			} else
				fullname = fullname + suffix;

			return FindType (type, fullname);
		}

		// Keep a dictionary cache of all types in a module rather than
		// looping through them on every lookup.
		TypeDefinition FindType (TypeDefinition type, string fullname)
		{
			Dictionary<string, TypeDefinition> types;

			if (!module_types.TryGetValue (type.Module, out types)) {
				types = GetTypesInModule (type.Module);
				module_types.Add (type.Module, types);
			}

			TypeDefinition helper;

			if (types.TryGetValue (fullname, out helper))
				return helper;

			return null;
		}

		static Dictionary<string, TypeDefinition> GetTypesInModule (ModuleDefinition module)
		{
			var types = module.Types.ToDictionary (p => p.FullName);

			foreach (var t in module.Types)
				AddNestedTypes (types, t);

			return types;
		}

		static void AddNestedTypes (Dictionary<string, TypeDefinition> types, TypeDefinition type)
		{
			if (!type.HasNestedTypes)
				return;

			foreach (var t in type.NestedTypes) {
				types.Add (t.FullName, t);
				AddNestedTypes (types, t);
			}
		}

		void PreserveConstructors (TypeDefinition type, TypeDefinition helper)
		{
			if (!helper.HasMethods)
				return;

			foreach (MethodDefinition ctor in helper.Methods.Where (m => m.IsConstructor))
				PreserveMethod (type, ctor);
		}

		static bool IsImplementor (TypeDefinition type)
		{
			return type.Name.EndsWith ("Implementor") && type.Inherits ("Java.Lang.Object");
		}

		static bool IsUserType (TypeDefinition type)
		{
			return !MonoAndroidHelper.IsFrameworkAssembly (type.Module.Assembly.Name.Name + ".dll");
		}

		void PreserveImplementor (TypeDefinition type)
		{
			if (!type.HasMethods)
				return;

			foreach (MethodDefinition method in type.Methods)
				if (method.Name.EndsWith ("Handler"))
					PreserveMethod (type, method);
		}
	}
}
